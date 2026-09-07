/*
 * gateway_settings — Settings v2 specialized codec (Phase 0).
 *
 * Encode/decode for Settings message families. All messages use
 * GW_MSG_TYPE_DEVICE_COMMAND as the wire type with Settings-specific
 * commands, keeping backward-compatible with Protocol v4 decoder.
 */
#include "gateway_settings.h"

#include <string.h>

#if defined(GW_HOST_TEST) || !defined(ESP_PLATFORM)
#include <stdio.h>
#define GW_LOGE(...)               \
    do {                           \
        fputs("GW_E: ", stderr);   \
        fprintf(stderr, __VA_ARGS__);   \
        fputc('\n', stderr);       \
    } while (0)
#else
#include "esp_log.h"
static const char *TAG = "gateway_settings";
#define GW_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#endif

/* ------------------------------------------------------------------ *
 * Internal CBOR writer
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
} gw_writer_t;

static int gw_put_byte(gw_writer_t *w, uint8_t byte)
{
    if (w->len >= w->cap) {
        w->len++;
        return GW_ERR_NO_SPACE;
    }
    w->buf[w->len++] = byte;
    return GW_OK;
}

static int gw_put_head(gw_writer_t *w, uint8_t major, uint64_t argument)
{
    int rc;
    uint8_t header = (uint8_t)(major << 5);

    if (argument <= 0x17ull) {
        rc = gw_put_byte(w, (uint8_t)(header | (uint8_t)argument));
    } else if (argument <= 0xffull) {
        rc = gw_put_byte(w, (uint8_t)(header | 24u));
        if (rc == GW_OK) rc = gw_put_byte(w, (uint8_t)argument);
    } else if (argument <= 0xffffull) {
        rc = gw_put_byte(w, (uint8_t)(header | 25u));
        if (rc == GW_OK) rc = gw_put_byte(w, (uint8_t)(argument >> 8));
        if (rc == GW_OK) rc = gw_put_byte(w, (uint8_t)argument);
    } else if (argument <= 0xffffffffull) {
        rc = gw_put_byte(w, (uint8_t)(header | 26u));
        for (int shift = 24; shift >= 0 && rc == GW_OK; shift -= 8) {
            rc = gw_put_byte(w, (uint8_t)(argument >> shift));
        }
    } else {
        rc = gw_put_byte(w, (uint8_t)(header | 27u));
        for (int shift = 56; shift >= 0 && rc == GW_OK; shift -= 8) {
            rc = gw_put_byte(w, (uint8_t)((uint64_t)argument >> shift));
        }
    }
    return rc;
}

static int gw_put_uint(gw_writer_t *w, uint64_t value)
{
    return gw_put_head(w, 0u, value);
}

static int gw_put_int(gw_writer_t *w, int64_t value)
{
    if (value >= 0) {
        return gw_put_head(w, 0u, (uint64_t)value);
    }
    return gw_put_head(w, 1u, (uint64_t)(-1ll - value));
}

static int gw_put_text(gw_writer_t *w, const char *text)
{
    size_t length = strlen(text);
    int rc = gw_put_head(w, 3u, (uint64_t)length);
    for (size_t i = 0; rc == GW_OK && i < length; i++) {
        rc = gw_put_byte(w, (uint8_t)text[i]);
    }
    return rc;
}

static int gw_put_bool(gw_writer_t *w, bool value)
{
    return gw_put_head(w, 7u, value ? 21u : 20u);
}

/* Emit actual Settings stream frame type (not device_command). */
static int gw_settings_put_stream_type(gw_writer_t *w, const char *msg_type,
                                       const char *command)
{
    int rc = gw_put_uint(w, GW_KEY_TYPE);
    if (rc == GW_OK) rc = gw_put_text(w, msg_type);
    if (rc == GW_OK) rc = gw_put_uint(w, GW_KEY_COMMAND);
    if (rc == GW_OK) rc = gw_put_text(w, command);
    return rc;
}

static int gw_settings_put_type_command(gw_writer_t *w, const char *command)
{
    return gw_settings_put_stream_type(w, GW_MSG_TYPE_DEVICE_COMMAND, command);
}

static int gw_settings_finish(gw_writer_t *w)
{
    if (w->len > GW_MSG_MAX_LEN || w->len > GW_SETTINGS_TARGET_ATT_PAYLOAD) {
        return GW_ERR_NO_SPACE;
    }
    return (int)w->len;
}

static int gw_settings_put_request_id(gw_writer_t *w, uint32_t request_id)
{
    int rc = gw_put_uint(w, GW_KEY_REQUEST_ID);
    if (rc == GW_OK) rc = gw_put_uint(w, request_id);
    return rc;
}

/* Required fields: int_value (0) and bool_value (false). */
static int gw_settings_put_required_fields(gw_writer_t *w)
{
    int rc = gw_put_uint(w, GW_KEY_INT_VALUE);
    if (rc == GW_OK) rc = gw_put_int(w, 0);
    if (rc == GW_OK) rc = gw_put_uint(w, GW_KEY_BOOL_VALUE);
    if (rc == GW_OK) rc = gw_put_bool(w, false);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Encode API
 * ------------------------------------------------------------------ */

int gw_settings_encode_begin(uint8_t *out_buf, size_t out_cap,
                             uint16_t total, uint32_t request_id)
{
    if (out_buf == NULL || out_cap == 0) return GW_ERR_INVALID_ARG;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, 7u); /* map(7) */
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_stream_type(&w, GW_MSG_TYPE_SETTINGS_BEGIN, GW_COMMAND_DESCRIBE_SETTINGS);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_TOTAL);
        if (rc == GW_OK) rc = gw_put_uint(&w, total);
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

int gw_settings_encode_item(uint8_t *out_buf, size_t out_cap,
                            uint16_t item_index, uint16_t total,
                            uint32_t request_id,
                            const char *setting_id,
                            const char *title,
                            const char *group,
                            const char *unit,
                            uint8_t type,
                            uint16_t flags,
                            uint16_t max_length,
                            int32_t min_value,
                            int32_t max_value,
                            uint32_t step)
{
    if (out_buf == NULL || out_cap == 0 || setting_id == NULL || title == NULL)
        return GW_ERR_INVALID_ARG;

    uint8_t pair_count = 12u;
    if (group != NULL && group[0] != '\0') pair_count++;
    if (unit != NULL && unit[0] != '\0') pair_count++;
    if (max_length > 0) pair_count++;
    if (type == GW_SETTING_TYPE_INT) pair_count += 3;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, pair_count);
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_stream_type(&w, GW_MSG_TYPE_SETTINGS_ITEM, GW_COMMAND_DESCRIBE_SETTINGS);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_SEQUENCE);
        if (rc == GW_OK) rc = gw_put_uint(&w, item_index);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_TOTAL);
        if (rc == GW_OK) rc = gw_put_uint(&w, total);
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_ID);
        if (rc == GW_OK) rc = gw_put_text(&w, setting_id);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_TITLE);
        if (rc == GW_OK) rc = gw_put_text(&w, title);
    }
    if (rc == GW_OK && group != NULL && group[0] != '\0') {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_GROUP);
        if (rc == GW_OK) rc = gw_put_text(&w, group);
    }
    if (rc == GW_OK && unit != NULL && unit[0] != '\0') {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_UNIT);
        if (rc == GW_OK) rc = gw_put_text(&w, unit);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_TYPE);
        if (rc == GW_OK) rc = gw_put_uint(&w, type);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_FLAGS);
        if (rc == GW_OK) rc = gw_put_uint(&w, flags);
    }
    if (rc == GW_OK && max_length > 0) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_MAX_LENGTH);
        if (rc == GW_OK) rc = gw_put_uint(&w, max_length);
    }
    if (rc == GW_OK && type == GW_SETTING_TYPE_INT) {
        rc = gw_put_uint(&w, GW_KEY_MIN_VALUE);
        if (rc == GW_OK) rc = gw_put_int(&w, min_value);
        if (rc == GW_OK) rc = gw_put_uint(&w, GW_KEY_MAX_VALUE);
        if (rc == GW_OK) rc = gw_put_int(&w, max_value);
        if (rc == GW_OK) rc = gw_put_uint(&w, GW_KEY_STEP);
        if (rc == GW_OK) rc = gw_put_uint(&w, step);
    }
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

int gw_settings_encode_option_item(uint8_t *out_buf, size_t out_cap,
                                   uint16_t item_index,
                                   uint8_t option_index,
                                   uint32_t request_id,
                                   const char *option_label)
{
    if (out_buf == NULL || out_cap == 0 || option_label == NULL)
        return GW_ERR_INVALID_ARG;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, 9u); /* map(9) */
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_stream_type(&w, GW_MSG_TYPE_SETTINGS_OPTION_ITEM, GW_COMMAND_DESCRIBE_SETTINGS);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_SEQUENCE);
        if (rc == GW_OK) rc = gw_put_uint(&w, item_index);
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_OPTION_INDEX);
        if (rc == GW_OK) rc = gw_put_uint(&w, option_index);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_TITLE);
        if (rc == GW_OK) rc = gw_put_text(&w, option_label);
    }
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

int gw_settings_encode_end(uint8_t *out_buf, size_t out_cap,
                           uint16_t total, uint32_t request_id)
{
    if (out_buf == NULL || out_cap == 0) return GW_ERR_INVALID_ARG;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, 7u); /* map(7): v4, type+cmd, total, req_id, int, bool */
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_stream_type(&w, GW_MSG_TYPE_SETTINGS_END, GW_COMMAND_DESCRIBE_SETTINGS);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_TOTAL);
        if (rc == GW_OK) rc = gw_put_uint(&w, total);
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

int gw_settings_encode_values_begin(uint8_t *out_buf, size_t out_cap,
                                    uint16_t total, uint32_t revision,
                                    uint32_t request_id)
{
    if (out_buf == NULL || out_cap == 0) return GW_ERR_INVALID_ARG;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, 8u); /* map(8) */
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_stream_type(&w, GW_MSG_TYPE_SETTINGS_VALUES_BEGIN, GW_COMMAND_GET_SETTINGS);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_TOTAL);
        if (rc == GW_OK) rc = gw_put_uint(&w, total);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_CAPABILITY_REVISION);
        if (rc == GW_OK) rc = gw_put_uint(&w, revision);
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

int gw_settings_encode_value(uint8_t *out_buf, size_t out_cap,
                             uint16_t item_index,
                             const char *setting_id,
                             uint8_t value_type,
                             const void *value,
                             uint32_t request_id)
{
    if (out_buf == NULL || out_cap == 0 || setting_id == NULL || value == NULL)
        return GW_ERR_INVALID_ARG;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, 10u); /* map(10) */
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_stream_type(&w, GW_MSG_TYPE_SETTINGS_VALUE, GW_COMMAND_GET_SETTINGS);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_SEQUENCE);
        if (rc == GW_OK) rc = gw_put_uint(&w, item_index);
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_ID);
        if (rc == GW_OK) rc = gw_put_text(&w, setting_id);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_TYPE);
        if (rc == GW_OK) rc = gw_put_uint(&w, value_type);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_VALUE);
        if (rc == GW_OK) {
            switch (value_type) {
            case GW_SETTING_TYPE_BOOL:
                rc = gw_put_bool(&w, *(const bool *)value);
                break;
            case GW_SETTING_TYPE_INT:
                rc = gw_put_int(&w, *(const int32_t *)value);
                break;
            case GW_SETTING_TYPE_STRING:
                rc = gw_put_text(&w, (const char *)value);
                break;
            case GW_SETTING_TYPE_ENUM:
                rc = gw_put_uint(&w, *(const uint8_t *)value);
                break;
            default:
                rc = GW_ERR_INVALID_ARG;
                break;
            }
        }
    }
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

int gw_settings_encode_values_end(uint8_t *out_buf, size_t out_cap,
                                  uint16_t total, uint32_t revision,
                                  uint32_t request_id)
{
    if (out_buf == NULL || out_cap == 0) return GW_ERR_INVALID_ARG;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, 8u); /* map(8): v4, type+cmd, total, rev, req_id, int, bool */
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_stream_type(&w, GW_MSG_TYPE_SETTINGS_VALUES_END, GW_COMMAND_GET_SETTINGS);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_TOTAL);
        if (rc == GW_OK) rc = gw_put_uint(&w, total);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_CAPABILITY_REVISION);
        if (rc == GW_OK) rc = gw_put_uint(&w, revision);
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

/* ------------------------------------------------------------------ *
 * Transaction encode API
 * ------------------------------------------------------------------ */

int gw_settings_encode_tx_begin(uint8_t *out_buf, size_t out_cap,
                                uint64_t transaction_id,
                                uint32_t expected_revision,
                                uint32_t request_id)
{
    if (out_buf == NULL || out_cap == 0) return GW_ERR_INVALID_ARG;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, 8u); /* map(8) */
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_type_command(
        &w, GW_MSG_TYPE_SETTINGS_TX_BEGIN);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_TRANSACTION_ID);
        if (rc == GW_OK) rc = gw_put_uint(&w, transaction_id);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_EXPECTED_REVISION);
        if (rc == GW_OK) rc = gw_put_uint(&w, expected_revision);
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

int gw_settings_encode_tx_set(uint8_t *out_buf, size_t out_cap,
                              uint64_t transaction_id,
                              const char *setting_id,
                              uint8_t value_type,
                              const void *value,
                              uint32_t request_id)
{
    if (out_buf == NULL || out_cap == 0 || setting_id == NULL || value == NULL)
        return GW_ERR_INVALID_ARG;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, 10u); /* map(10) */
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_type_command(
        &w, GW_MSG_TYPE_SETTINGS_TX_SET);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_TRANSACTION_ID);
        if (rc == GW_OK) rc = gw_put_uint(&w, transaction_id);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_ID);
        if (rc == GW_OK) rc = gw_put_text(&w, setting_id);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_TYPE);
        if (rc == GW_OK) rc = gw_put_uint(&w, value_type);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_VALUE);
        if (rc == GW_OK) {
            switch (value_type) {
            case GW_SETTING_TYPE_BOOL:
                rc = gw_put_bool(&w, *(const bool *)value);
                break;
            case GW_SETTING_TYPE_INT:
                rc = gw_put_int(&w, *(const int32_t *)value);
                break;
            case GW_SETTING_TYPE_STRING:
                rc = gw_put_text(&w, (const char *)value);
                break;
            case GW_SETTING_TYPE_ENUM:
                rc = gw_put_uint(&w, *(const uint8_t *)value);
                break;
            default:
                rc = GW_ERR_INVALID_ARG;
                break;
            }
        }
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

int gw_settings_encode_tx_commit(uint8_t *out_buf, size_t out_cap,
                                 uint64_t transaction_id,
                                 uint32_t request_id)
{
    if (out_buf == NULL || out_cap == 0) return GW_ERR_INVALID_ARG;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, 7u); /* map(7): v4, type+cmd, tx_id, req_id, int, bool */
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_type_command(
        &w, GW_MSG_TYPE_SETTINGS_TX_COMMIT);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_TRANSACTION_ID);
        if (rc == GW_OK) rc = gw_put_uint(&w, transaction_id);
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

int gw_settings_encode_tx_abort(uint8_t *out_buf, size_t out_cap,
                                uint64_t transaction_id,
                                uint32_t request_id)
{
    if (out_buf == NULL || out_cap == 0) return GW_ERR_INVALID_ARG;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, 7u); /* map(7): v4, type+cmd, tx_id, req_id, int, bool */
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_type_command(
        &w, GW_MSG_TYPE_SETTINGS_TX_ABORT);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_TRANSACTION_ID);
        if (rc == GW_OK) rc = gw_put_uint(&w, transaction_id);
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

int gw_settings_encode_commit_confirm(uint8_t *out_buf, size_t out_cap,
                                      uint64_t transaction_id,
                                      uint32_t new_revision,
                                      uint32_t request_id)
{
    if (out_buf == NULL || out_cap == 0) return GW_ERR_INVALID_ARG;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, 8u); /* map(8) */
    if (rc != GW_OK) return rc;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, GW_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_settings_put_type_command(&w, GW_MSG_TYPE_SETTINGS_COMMIT_CONFIRM);
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_TRANSACTION_ID);
        if (rc == GW_OK) rc = gw_put_uint(&w, transaction_id);
    }
    if (rc == GW_OK) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_NEW_REVISION);
        if (rc == GW_OK) rc = gw_put_uint(&w, new_revision);
    }
    if (rc == GW_OK) rc = gw_settings_put_request_id(&w, request_id);
    if (rc == GW_OK) rc = gw_settings_put_required_fields(&w);

    if (rc != GW_OK) return rc;
    return gw_settings_finish(&w);
}

/* ------------------------------------------------------------------ *
 * Decode API
 * ------------------------------------------------------------------ */

int gw_settings_decode_command(const gw_message_t *msg,
                               gw_settings_command_t *out_cmd)
{
    if (msg == NULL || out_cmd == NULL) return GW_ERR_INVALID_ARG;
    if (msg->protocol_version != GW_PROTOCOL_VERSION) return GW_ERR_UNSUPPORTED_VERSION;

    memset(out_cmd, 0, sizeof(*out_cmd));

    if (strcmp(msg->command, GW_COMMAND_DESCRIBE_SETTINGS) == 0) {
        out_cmd->cmd_type = GW_SETTINGS_CMD_DESCRIBE;
    } else if (strcmp(msg->command, GW_COMMAND_READ_SETTINGS) == 0) {
        out_cmd->cmd_type = GW_SETTINGS_CMD_READ;
    } else if (strcmp(msg->command, GW_MSG_TYPE_SETTINGS_TX_BEGIN) == 0) {
        out_cmd->cmd_type = GW_SETTINGS_CMD_TX_BEGIN;
    } else if (strcmp(msg->command, GW_MSG_TYPE_SETTINGS_TX_SET) == 0) {
        out_cmd->cmd_type = GW_SETTINGS_CMD_TX_SET;
    } else if (strcmp(msg->command, GW_MSG_TYPE_SETTINGS_TX_COMMIT) == 0) {
        out_cmd->cmd_type = GW_SETTINGS_CMD_TX_COMMIT;
    } else if (strcmp(msg->command, GW_MSG_TYPE_SETTINGS_TX_ABORT) == 0) {
        out_cmd->cmd_type = GW_SETTINGS_CMD_TX_ABORT;
    } else if (strcmp(msg->command, GW_MSG_TYPE_SETTINGS_COMMIT_CONFIRM) == 0) {
        out_cmd->cmd_type = GW_SETTINGS_CMD_TX_CONFIRM;
    } else {
        return GW_ERR_DECODE;
    }

    if (msg->has_request_id && msg->request_id > 0) {
        out_cmd->request_id = msg->request_id;
        out_cmd->has_request_id = 1;
    }
    if (msg->has_settings_transaction_id) {
        out_cmd->transaction_id = msg->settings_transaction_id;
        out_cmd->has_transaction_id = 1;
    }
    if (msg->has_settings_expected_revision) {
        out_cmd->expected_revision = msg->settings_expected_revision;
        out_cmd->has_expected_revision = 1;
    }
    if (msg->has_setting_id) {
        strlcpy(out_cmd->setting_id, msg->setting_id, sizeof(out_cmd->setting_id));
        out_cmd->has_setting_id = 1;
    }
    if (msg->has_setting_value) {
        out_cmd->setting_value.type = msg->setting_value_type;
        switch (msg->setting_value_type) {
        case GW_SETTING_TYPE_BOOL:
            out_cmd->setting_value.value.bool_val =
                msg->setting_value.bool_val;
            break;
        case GW_SETTING_TYPE_INT:
            out_cmd->setting_value.value.int_val =
                msg->setting_value.int_val;
            break;
        case GW_SETTING_TYPE_STRING:
            strlcpy(out_cmd->setting_value.value.str_val,
                    msg->setting_value.str_val,
                    sizeof(out_cmd->setting_value.value.str_val));
            break;
        case GW_SETTING_TYPE_ENUM:
            out_cmd->setting_value.value.enum_val =
                msg->setting_value.enum_val;
            break;
        default:
            return GW_ERR_DECODE;
        }
        out_cmd->has_setting_value = 1;
    }
    if (msg->has_settings_new_revision) {
        out_cmd->new_revision = msg->settings_new_revision;
        out_cmd->has_new_revision = 1;
    }

    return GW_OK;
}