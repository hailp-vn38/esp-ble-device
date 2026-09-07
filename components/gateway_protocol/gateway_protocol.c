/*
 * gateway_protocol — CBOR codec for ESP-GATT Protocol v4 (Device side).
 *
 * Wire format: definite-length CBOR map with numeric keys, mirroring the
 * Gateway encoder (QCBOR) and decoder semantics:
 *  - required RX fields: protocol_version(0) == 4, type(1), command(3),
 *    int_value(4), bool_value(5)
 *  - any other protocol version (or absent version) is rejected
 *  - request_id(10) optional; if present must be 1..UINT32_MAX
 *  - key 7 is reserved: never emitted, ignored on decode
 *  - unknown keys tolerated (skipped); no trailing bytes allowed
 *
 * Encoder always emits explicit protocol_version (v4) per strict-v4
 * contract: no version fallback exists on either side.
 */
#include "gateway_protocol.h"

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
static const char *TAG = "gateway_protocol";
#define GW_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#endif

#define GW_CBOR_MAX_DEPTH 8

const char *gw_result_str(int result)
{
    switch ((gw_result_t)result) {
    case GW_OK: return "GW_OK";
    case GW_ERR_INVALID_ARG: return "GW_ERR_INVALID_ARG";
    case GW_ERR_NO_SPACE: return "GW_ERR_NO_SPACE";
    case GW_ERR_ENCODE: return "GW_ERR_ENCODE";
    case GW_ERR_DECODE: return "GW_ERR_DECODE";
    case GW_ERR_UNSUPPORTED_VERSION: return "GW_ERR_UNSUPPORTED_VERSION";
    case GW_ERR_VALIDATION: return "GW_ERR_VALIDATION";
    default: return "GW_ERR_UNKNOWN";
    }
}

void gw_message_init(gw_message_t *msg)
{
    if (msg == NULL) return;
    memset(msg, 0, sizeof(*msg));
    msg->protocol_version = GW_PROTOCOL_VERSION;
}

uint16_t gw_ble_max_tx_payload(uint16_t negotiated_mtu)
{
    uint16_t att_payload =
        (uint16_t)(negotiated_mtu > 3u ? negotiated_mtu - 3u : 0u);
    return (att_payload < GW_MSG_MAX_LEN) ? att_payload : (uint16_t)GW_MSG_MAX_LEN;
}

/* ================================================================== *
 * Encoding
 * ================================================================== */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
} gw_writer_t;

static int gw_put_byte(gw_writer_t *w, uint8_t byte)
{
    if (w->len >= w->cap) {
        w->len++; /* keep counting so overflow is detected deterministically */
        return GW_ERR_NO_SPACE;
    }
    w->buf[w->len++] = byte;
    return GW_OK;
}

/* Write major type + argument using minimal-length encoding. */
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

static int gw_put_bytes(gw_writer_t *w, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    int rc = gw_put_head(w, 2u, (uint64_t)length);
    for (size_t i = 0; rc == GW_OK && i < length; i++) {
        rc = gw_put_byte(w, bytes[i]);
    }
    return rc;
}

static bool gw_string_fits(const char *value, size_t capacity)
{
    return strnlen(value, capacity) < capacity;
}

/* Bounded copy, always NUL-terminates (portable strlcpy). */
static void gw_copy_str(char *destination, size_t capacity, const char *source)
{
    size_t length = strnlen(source, capacity - 1u);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

int gw_message_encode(const gw_message_t *msg, uint8_t *out_buf, size_t out_cap)
{
    if (msg == NULL || out_buf == NULL || out_cap == 0 ||
        !gw_string_fits(msg->type, sizeof(msg->type)) ||
        msg->type[0] == '\0' ||
        !gw_string_fits(msg->command, sizeof(msg->command)) ||
        msg->command[0] == '\0' ||
        !gw_string_fits(msg->capability_label,
                        sizeof(msg->capability_label)) ||
        !gw_string_fits(msg->capability_unit,
                        sizeof(msg->capability_unit)) ||
        !gw_string_fits(msg->feature_id, sizeof(msg->feature_id)) ||
        !gw_string_fits(msg->feature_tool, sizeof(msg->feature_tool))) {
        return GW_ERR_INVALID_ARG;
    }

    /* Contract: firmware emits exactly v4. Any other version (including
     * zero/absent semantics) is rejected before touching the buffer. */
    uint64_t version = msg->protocol_version;
    if (version != (uint64_t)GW_PROTOCOL_VERSION) {
        GW_LOGE("encode: unsupported protocol_version %llu",
                (unsigned long long)version);
        return GW_ERR_UNSUPPORTED_VERSION;
    }

    if (msg->has_request_id && msg->request_id == 0u) {
        return GW_ERR_VALIDATION; /* request_id 0 is not wire-valid */
    }
    if (msg->has_snapshot_id && msg->snapshot_id == 0u) {
        return GW_ERR_VALIDATION;
    }
    if (msg->has_device_id &&
        (!gw_string_fits(msg->device_id, sizeof(msg->device_id)) ||
         msg->device_id[0] == '\0')) {
        return GW_ERR_VALIDATION;
    }
    if (!gw_string_fits(msg->name, sizeof(msg->name))) {
        return GW_ERR_VALIDATION;
    }

    unsigned pair_count = 5u; /* version, type, command, int_value, bool_value */
    if (msg->has_device_id) pair_count++;
    if (msg->has_request_id) pair_count++;
    if (msg->name[0] != '\0') pair_count++;
    if (msg->has_ble_addr) pair_count += 2;
    if (msg->has_snapshot_id) pair_count++;
    if (msg->has_sequence) pair_count++;
    if (msg->has_total) pair_count++;
    if (msg->has_value_type) pair_count++;
    if (msg->has_capability_flags) pair_count++;
    if (msg->has_min_value) pair_count++;
    if (msg->has_max_value) pair_count++;
    if (msg->has_step) pair_count++;
    if (msg->capability_label[0] != '\0') pair_count++;
    if (msg->capability_unit[0] != '\0') pair_count++;
    if (msg->has_capability_revision) pair_count++;
    if (msg->has_feature_id) pair_count++;
    if (msg->has_feature_type) pair_count++;
    if (msg->has_feature_schema_version) pair_count++;
    if (msg->has_feature_flags) pair_count++;
    if (msg->has_property_id) pair_count++;
    if (msg->has_feature_value_bool) pair_count++;
    if (msg->has_feature_value_int) pair_count++;
    if (msg->has_feature_tool) pair_count++;
    if (msg->has_feature_total) pair_count++;
    if (msg->has_feature_decimals) pair_count++;
    if (msg->has_settings_supported) pair_count++;
    if (msg->has_settings_schema_revision) pair_count++;

    gw_writer_t w = { out_buf, out_cap, 0u };
    int rc = gw_put_head(&w, 5u, pair_count);
    if (rc != GW_OK) goto done;

    rc = gw_put_uint(&w, GW_KEY_PROTOCOL_VERSION);
    if (rc == GW_OK) rc = gw_put_uint(&w, version);

    if (rc == GW_OK) rc = gw_put_uint(&w, GW_KEY_TYPE);
    if (rc == GW_OK) rc = gw_put_text(&w, msg->type);

    if (rc == GW_OK && msg->has_device_id) {
        rc = gw_put_uint(&w, GW_KEY_DEVICE_ID);
        if (rc == GW_OK) rc = gw_put_text(&w, msg->device_id);
    }

    if (rc == GW_OK) rc = gw_put_uint(&w, GW_KEY_COMMAND);
    if (rc == GW_OK) rc = gw_put_text(&w, msg->command);

    if (rc == GW_OK && msg->has_request_id) {
        rc = gw_put_uint(&w, GW_KEY_REQUEST_ID);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->request_id);
    }

    if (rc == GW_OK) rc = gw_put_uint(&w, GW_KEY_INT_VALUE);
    if (rc == GW_OK) rc = gw_put_int(&w, (int64_t)msg->int_value);

    if (rc == GW_OK) rc = gw_put_uint(&w, GW_KEY_BOOL_VALUE);
    if (rc == GW_OK) rc = gw_put_head(&w, 7u, msg->bool_value ? 21u : 20u);

    if (rc == GW_OK && msg->name[0] != '\0') {
        rc = gw_put_uint(&w, GW_KEY_NAME);
        if (rc == GW_OK) rc = gw_put_text(&w, msg->name);
    }

    /* Key 7 (GW_KEY_RESERVED_7) is never emitted since v4. */

    if (rc == GW_OK && msg->has_ble_addr) {
        rc = gw_put_uint(&w, GW_KEY_BLE_ADDR);
        if (rc == GW_OK) {
            rc = gw_put_bytes(&w, msg->ble_addr, sizeof(msg->ble_addr));
        }
        if (rc == GW_OK) rc = gw_put_uint(&w, GW_KEY_BLE_ADDR_TYPE);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->ble_addr_type);
    }

    if (rc == GW_OK && msg->has_snapshot_id) {
        rc = gw_put_uint(&w, GW_KEY_SNAPSHOT_ID);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->snapshot_id);
    }
    if (rc == GW_OK && msg->has_sequence) {
        rc = gw_put_uint(&w, GW_KEY_SEQUENCE);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->sequence);
    }
    if (rc == GW_OK && msg->has_total) {
        rc = gw_put_uint(&w, GW_KEY_TOTAL);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->total);
    }
    if (rc == GW_OK && msg->has_value_type) {
        rc = gw_put_uint(&w, GW_KEY_VALUE_TYPE);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->value_type);
    }
    if (rc == GW_OK && msg->has_capability_flags) {
        rc = gw_put_uint(&w, GW_KEY_CAPABILITY_FLAGS);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->capability_flags);
    }
    if (rc == GW_OK && msg->has_min_value) {
        rc = gw_put_uint(&w, GW_KEY_MIN_VALUE);
        if (rc == GW_OK) rc = gw_put_int(&w, msg->min_value);
    }
    if (rc == GW_OK && msg->has_max_value) {
        rc = gw_put_uint(&w, GW_KEY_MAX_VALUE);
        if (rc == GW_OK) rc = gw_put_int(&w, msg->max_value);
    }
    if (rc == GW_OK && msg->has_step) {
        rc = gw_put_uint(&w, GW_KEY_STEP);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->step);
    }
    if (rc == GW_OK && msg->capability_label[0] != '\0') {
        rc = gw_put_uint(&w, GW_KEY_CAPABILITY_LABEL);
        if (rc == GW_OK) rc = gw_put_text(&w, msg->capability_label);
    }
    if (rc == GW_OK && msg->capability_unit[0] != '\0') {
        rc = gw_put_uint(&w, GW_KEY_CAPABILITY_UNIT);
        if (rc == GW_OK) rc = gw_put_text(&w, msg->capability_unit);
    }
    if (rc == GW_OK && msg->has_capability_revision) {
        rc = gw_put_uint(&w, GW_KEY_CAPABILITY_REVISION);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->capability_revision);
    }
    if (rc == GW_OK && msg->has_feature_id) {
        rc = gw_put_uint(&w, GW_KEY_FEATURE_ID);
        if (rc == GW_OK) rc = gw_put_text(&w, msg->feature_id);
    }
    if (rc == GW_OK && msg->has_feature_type) {
        rc = gw_put_uint(&w, GW_KEY_FEATURE_TYPE);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->feature_type);
    }
    if (rc == GW_OK && msg->has_feature_schema_version) {
        rc = gw_put_uint(&w, GW_KEY_FEATURE_SCHEMA_VERSION);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->feature_schema_version);
    }
    if (rc == GW_OK && msg->has_feature_flags) {
        rc = gw_put_uint(&w, GW_KEY_FEATURE_FLAGS);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->feature_flags);
    }
    if (rc == GW_OK && msg->has_property_id) {
        rc = gw_put_uint(&w, GW_KEY_PROPERTY_ID);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->property_id);
    }
    if (rc == GW_OK && msg->has_feature_value_bool) {
        rc = gw_put_uint(&w, GW_KEY_FEATURE_VALUE_BOOL);
        if (rc == GW_OK) rc = gw_put_head(&w, 7u,
                                           msg->feature_value_bool ? 21u : 20u);
    }
    if (rc == GW_OK && msg->has_feature_value_int) {
        rc = gw_put_uint(&w, GW_KEY_FEATURE_VALUE_INT);
        if (rc == GW_OK) rc = gw_put_int(&w, msg->feature_value_int);
    }
    if (rc == GW_OK && msg->has_feature_tool) {
        rc = gw_put_uint(&w, GW_KEY_FEATURE_TOOL);
        if (rc == GW_OK) rc = gw_put_text(&w, msg->feature_tool);
    }
    if (rc == GW_OK && msg->has_feature_total) {
        rc = gw_put_uint(&w, GW_KEY_FEATURE_TOTAL);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->feature_total);
    }
    if (rc == GW_OK && msg->has_feature_decimals) {
        rc = gw_put_uint(&w, GW_KEY_FEATURE_DECIMALS);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->feature_decimals);
    }
    if (rc == GW_OK && msg->has_settings_supported) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_SUPPORTED);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->settings_supported ? 1u : 0u);
    }
    if (rc == GW_OK && msg->has_settings_schema_revision) {
        rc = gw_put_uint(&w, GW_KEY_SETTINGS_SCHEMA_REVISION);
        if (rc == GW_OK) rc = gw_put_uint(&w, msg->settings_schema_revision);
    }

done:
    if (rc != GW_OK) return rc;
    if (w.len > GW_MSG_MAX_LEN) {
        GW_LOGE("encode: %zu bytes exceeds protocol max %u", w.len,
                (unsigned)GW_MSG_MAX_LEN);
        return GW_ERR_NO_SPACE;
    }
    return (int)w.len;
}

/* ================================================================== *
 * Decoding (strict single pass)
 * ================================================================== */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t off;
} gw_reader_t;

static int gw_get_byte(gw_reader_t *r, uint8_t *out)
{
    if (r->off >= r->len) return GW_ERR_DECODE;
    *out = r->data[r->off++];
    return GW_OK;
}

static int gw_get_raw(gw_reader_t *r, size_t count)
{
    if (count > r->len - r->off) return GW_ERR_DECODE;
    r->off += count;
    return GW_OK;
}

/* Read an item head. Rejects indefinite lengths (ai 31) and reserved ai
 * values (28..30): the Gateway QCBOR encoder never emits them. */
static int gw_get_head(gw_reader_t *r, uint8_t *out_major, uint64_t *out_arg)
{
    uint8_t initial;
    int rc = gw_get_byte(r, &initial);
    if (rc != GW_OK) return rc;

    uint8_t major = (uint8_t)(initial >> 5);
    uint8_t info = (uint8_t)(initial & 0x1fu);
    uint64_t argument = 0;

    switch (info) {
    case 24: {
        uint8_t byte = 0;
        rc = gw_get_byte(r, &byte);
        argument = byte;
        break;
    }
    case 25: {
        uint8_t hi = 0, lo = 0;
        rc = gw_get_byte(r, &hi);
        if (rc == GW_OK) rc = gw_get_byte(r, &lo);
        argument = ((uint64_t)hi << 8) | lo;
        break;
    }
    case 26: {
        uint64_t value = 0;
        for (int i = 0; rc == GW_OK && i < 4; i++) {
            uint8_t byte = 0;
            rc = gw_get_byte(r, &byte);
            value = (value << 8) | byte;
        }
        argument = value;
        break;
    }
    case 27: {
        uint64_t value = 0;
        for (int i = 0; rc == GW_OK && i < 8; i++) {
            uint8_t byte = 0;
            rc = gw_get_byte(r, &byte);
            value = (value << 8) | byte;
        }
        argument = value;
        break;
    }
    default:
        if (info >= 28u) return GW_ERR_DECODE;
        argument = info;
        break;
    }

    if (rc == GW_OK) {
        *out_major = major;
        *out_arg = argument;
    }
    return rc;
}

/* Bounds-checked payload copy for strings/bytes at reader position. */
static int gw_get_payload(gw_reader_t *r, uint64_t length, uint8_t *out,
                          size_t out_cap)
{
    if (length > (uint64_t)(r->len - r->off)) return GW_ERR_DECODE;
    if (length > (uint64_t)out_cap) return GW_ERR_DECODE;
    memcpy(out, &r->data[r->off], (size_t)length);
    r->off += (size_t)length;
    return GW_OK;
}

/* Structurally skip any well-formed definite-length item. */
static int gw_skip_item(gw_reader_t *r, int depth)
{
    if (depth > GW_CBOR_MAX_DEPTH) return GW_ERR_DECODE;

    uint8_t major;
    uint64_t argument;
    int rc = gw_get_head(r, &major, &argument);
    if (rc != GW_OK) return rc;

    switch (major) {
    case 0:
    case 1:
    case 7:
        return GW_OK;
    case 2:
    case 3:
        return gw_get_raw(r, (size_t)argument);
    case 4:
        for (uint64_t i = 0; rc == GW_OK && i < argument; i++) {
            rc = gw_skip_item(r, depth + 1);
        }
        return rc;
    case 5:
        for (uint64_t i = 0; rc == GW_OK && i < argument * 2ull; i++) {
            rc = gw_skip_item(r, depth + 1);
        }
        return rc;
    default:
        return GW_ERR_DECODE;
    }
}

/* Read unsigned integer item; fails on negative or non-integer majors. */
static int gw_get_uint_bounded(gw_reader_t *r, uint64_t max_value,
                               uint64_t *out)
{
    uint8_t major;
    uint64_t argument;
    int rc = gw_get_head(r, &major, &argument);
    if (rc != GW_OK) return rc;
    if (major != 0u || argument > max_value) return GW_ERR_DECODE;
    *out = argument;
    return GW_OK;
}

/* Read signed integer item (major 0 or 1), bounded to int32 range. */
static int gw_get_int_value(gw_reader_t *r, int *out)
{
    uint8_t major;
    uint64_t argument;
    int rc = gw_get_head(r, &major, &argument);
    if (rc != GW_OK) return rc;

    if (major == 0u) {
        if (argument > (uint64_t)INT32_MAX) return GW_ERR_DECODE;
        *out = (int)argument;
        return GW_OK;
    }
    if (major == 1u) {
        /* CBOR negative: value = -1 - argument. Argument <= INT32_MAX
         * keeps the result >= INT32_MIN. */
        if (argument > (uint64_t)INT32_MAX) return GW_ERR_DECODE;
        *out = (int)(-1ll - (int64_t)argument);
        return GW_OK;
    }
    return GW_ERR_DECODE;
}

/* Read definite-length text string into fixed buffer with NUL. */
static int gw_get_text(gw_reader_t *r, char *out, size_t capacity,
                       bool allow_empty)
{
    uint8_t major;
    uint64_t argument;
    int rc = gw_get_head(r, &major, &argument);
    if (rc != GW_OK) return rc;
    if (major != 3u) return GW_ERR_DECODE;
    if (argument >= (uint64_t)capacity) return GW_ERR_DECODE;
    if (!allow_empty && argument == 0u) return GW_ERR_DECODE;

    rc = gw_get_payload(r, argument, (uint8_t *)out, capacity - 1u);
    if (rc != GW_OK) return rc;
    out[argument] = '\0';
    return GW_OK;
}

static int gw_get_bool(gw_reader_t *r, bool *out)
{
    uint8_t major;
    uint64_t argument;
    int rc = gw_get_head(r, &major, &argument);
    if (rc != GW_OK) return rc;
    if (major != 7u || (argument != 20u && argument != 21u)) {
        return GW_ERR_DECODE;
    }
    *out = (argument == 21u);
    return GW_OK;
}

static int gw_decode_setting_value(const uint8_t *data, size_t len,
                                   uint8_t type, gw_message_t *out_msg)
{
    if (data == NULL || out_msg == NULL) return GW_ERR_INVALID_ARG;

    gw_reader_t value_reader = { data, len, 0u };
    int rc;
    switch (type) {
    case GW_SETTING_TYPE_BOOL:
        rc = gw_get_bool(&value_reader, &out_msg->setting_value.bool_val);
        break;
    case GW_SETTING_TYPE_INT: {
        int value = 0;
        rc = gw_get_int_value(&value_reader, &value);
        if (rc == GW_OK) out_msg->setting_value.int_val = (int32_t)value;
        break;
    }
    case GW_SETTING_TYPE_STRING:
        rc = gw_get_text(&value_reader, out_msg->setting_value.str_val,
                         sizeof(out_msg->setting_value.str_val), true);
        break;
    case GW_SETTING_TYPE_ENUM: {
        uint64_t value = 0;
        rc = gw_get_uint_bounded(&value_reader, UINT8_MAX, &value);
        if (rc == GW_OK) out_msg->setting_value.enum_val = (uint8_t)value;
        break;
    }
    default:
        return GW_ERR_DECODE;
    }
    if (rc != GW_OK || value_reader.off != len) return GW_ERR_DECODE;

    out_msg->setting_value_type = type;
    out_msg->has_setting_value = 1;
    return GW_OK;
}

int gw_message_decode(const uint8_t *buf, size_t len, gw_message_t *out_msg)
{
    if (buf == NULL || out_msg == NULL || len == 0u || len > GW_MSG_MAX_LEN) {
        return GW_ERR_INVALID_ARG;
    }

    gw_message_init(out_msg);

    gw_reader_t r = { buf, len, 0u };

    uint8_t major;
    uint64_t pair_count;
    int rc = gw_get_head(&r, &major, &pair_count);
    if (rc != GW_OK) return rc;
    if (major != 5u) return GW_ERR_DECODE;
    /* Every pair consumes at least two one-byte items. */
    if (pair_count > (uint64_t)len) return GW_ERR_DECODE;

    uint64_t version = 0;
    bool has_version = false;
    bool has_type = false;
    bool has_command = false;
    bool has_int_value = false;
    bool has_bool_value = false;
    bool has_request_id = false;
    bool has_device_id = false;
    bool has_ble_addr = false;
    bool has_ble_addr_type = false;
    int int_value = 0;
    bool bool_value = false;
    uint64_t request_id = 0;
    uint8_t raw_setting_value[GW_MSG_MAX_LEN];
    size_t raw_setting_value_len = 0u;
    bool has_raw_setting_value = false;

    while (pair_count-- > 0ull && rc == GW_OK) {
        uint64_t key;
        rc = gw_get_uint_bounded(&r, UINT16_MAX, &key);
        if (rc != GW_OK) break;

        switch (key) {
        case GW_KEY_PROTOCOL_VERSION:
            rc = gw_get_uint_bounded(&r, UINT8_MAX, &version);
            if (rc == GW_OK) has_version = true;
            break;

        case GW_KEY_TYPE:
            rc = gw_get_text(&r, out_msg->type, sizeof(out_msg->type), false);
            if (rc == GW_OK) has_type = true;
            break;

        case GW_KEY_DEVICE_ID:
            rc = gw_get_text(&r, out_msg->device_id,
                             sizeof(out_msg->device_id), false);
            if (rc == GW_OK) has_device_id = true;
            break;

        case GW_KEY_COMMAND:
            rc = gw_get_text(&r, out_msg->command,
                             sizeof(out_msg->command), false);
            if (rc == GW_OK) has_command = true;
            break;

        case GW_KEY_INT_VALUE:
            rc = gw_get_int_value(&r, &int_value);
            if (rc == GW_OK) has_int_value = true;
            break;

        case GW_KEY_BOOL_VALUE:
            rc = gw_get_bool(&r, &bool_value);
            if (rc == GW_OK) has_bool_value = true;
            break;

        case GW_KEY_NAME:
            rc = gw_get_text(&r, out_msg->name, sizeof(out_msg->name), true);
            break;

        /* GW_KEY_RESERVED_7: skip value, keep numeric contract stable. */

        case GW_KEY_BLE_ADDR: {
            uint8_t bytes_major = 0;
            uint64_t bytes_len = 0;
            rc = gw_get_head(&r, &bytes_major, &bytes_len);
            if (rc != GW_OK) break;
            if (bytes_major != 2u || bytes_len != sizeof(out_msg->ble_addr)) {
                rc = GW_ERR_DECODE;
                break;
            }
            rc = gw_get_payload(&r, bytes_len, out_msg->ble_addr,
                                sizeof(out_msg->ble_addr));
            if (rc == GW_OK) has_ble_addr = true;
            break;
        }

        case GW_KEY_BLE_ADDR_TYPE: {
            uint64_t addr_type = 0;
            rc = gw_get_uint_bounded(&r, UINT8_MAX, &addr_type);
            if (rc != GW_OK) break;
            out_msg->ble_addr_type = (uint8_t)addr_type;
            has_ble_addr_type = true;
            break;
        }

        case GW_KEY_REQUEST_ID:
            rc = gw_get_uint_bounded(&r, UINT32_MAX, &request_id);
            if (rc != GW_OK) break;
            if (request_id == 0u) { /* contract: 1 <= request_id */
                rc = GW_ERR_DECODE;
                break;
            }
            has_request_id = true;
            break;

        case GW_KEY_SNAPSHOT_ID: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT32_MAX, &value);
            if (rc == GW_OK && value == 0u) rc = GW_ERR_DECODE;
            if (rc == GW_OK) {
                out_msg->snapshot_id = (uint32_t)value;
                out_msg->has_snapshot_id = 1;
            }
            break;
        }

        case GW_KEY_SEQUENCE: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT16_MAX, &value);
            if (rc == GW_OK) {
                out_msg->sequence = (uint16_t)value;
                out_msg->has_sequence = 1;
            }
            break;
        }

        case GW_KEY_TOTAL: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT16_MAX, &value);
            if (rc == GW_OK) {
                out_msg->total = (uint16_t)value;
                out_msg->has_total = 1;
            }
            break;
        }

        case GW_KEY_VALUE_TYPE: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT8_MAX, &value);
            if (rc == GW_OK) {
                out_msg->value_type = (uint8_t)value;
                out_msg->has_value_type = 1;
            }
            break;
        }

        case GW_KEY_CAPABILITY_FLAGS: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT8_MAX, &value);
            if (rc == GW_OK) {
                out_msg->capability_flags = (uint8_t)value;
                out_msg->has_capability_flags = 1;
            }
            break;
        }

        case GW_KEY_MIN_VALUE: {
            int value = 0;
            rc = gw_get_int_value(&r, &value);
            if (rc == GW_OK) {
                out_msg->min_value = (int32_t)value;
                out_msg->has_min_value = 1;
            }
            break;
        }

        case GW_KEY_MAX_VALUE: {
            int value = 0;
            rc = gw_get_int_value(&r, &value);
            if (rc == GW_OK) {
                out_msg->max_value = (int32_t)value;
                out_msg->has_max_value = 1;
            }
            break;
        }

        case GW_KEY_STEP: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT32_MAX, &value);
            if (rc == GW_OK) {
                out_msg->step = (uint32_t)value;
                out_msg->has_step = 1;
            }
            break;
        }

        case GW_KEY_CAPABILITY_LABEL:
            rc = gw_get_text(&r, out_msg->capability_label,
                             sizeof(out_msg->capability_label), true);
            break;

        case GW_KEY_CAPABILITY_UNIT:
            rc = gw_get_text(&r, out_msg->capability_unit,
                             sizeof(out_msg->capability_unit), true);
            break;

        case GW_KEY_CAPABILITY_REVISION: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT32_MAX, &value);
            if (rc == GW_OK) {
                out_msg->capability_revision = (uint32_t)value;
                out_msg->has_capability_revision = 1;
            }
            break;
        }

        case GW_KEY_FEATURE_ID:
            rc = gw_get_text(&r, out_msg->feature_id,
                             sizeof(out_msg->feature_id), false);
            if (rc == GW_OK) out_msg->has_feature_id = 1;
            break;

        case GW_KEY_FEATURE_TYPE: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT8_MAX, &value);
            if (rc == GW_OK) {
                out_msg->feature_type = (uint8_t)value;
                out_msg->has_feature_type = 1;
            }
            break;
        }

        case GW_KEY_FEATURE_SCHEMA_VERSION: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT16_MAX, &value);
            if (rc == GW_OK) {
                out_msg->feature_schema_version = (uint16_t)value;
                out_msg->has_feature_schema_version = 1;
            }
            break;
        }

        case GW_KEY_FEATURE_FLAGS: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT16_MAX, &value);
            if (rc == GW_OK) {
                out_msg->feature_flags = (uint16_t)value;
                out_msg->has_feature_flags = 1;
            }
            break;
        }

        case GW_KEY_PROPERTY_ID: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT8_MAX, &value);
            if (rc == GW_OK) {
                out_msg->property_id = (uint8_t)value;
                out_msg->has_property_id = 1;
            }
            break;
        }

        case GW_KEY_FEATURE_VALUE_BOOL: {
            bool value = false;
            rc = gw_get_bool(&r, &value);
            if (rc == GW_OK) {
                out_msg->feature_value_bool = value;
                out_msg->has_feature_value_bool = 1;
            }
            break;
        }

        case GW_KEY_FEATURE_VALUE_INT: {
            int value = 0;
            rc = gw_get_int_value(&r, &value);
            if (rc == GW_OK) {
                out_msg->feature_value_int = (int32_t)value;
                out_msg->has_feature_value_int = 1;
            }
            break;
        }

        case GW_KEY_FEATURE_TOOL:
            rc = gw_get_text(&r, out_msg->feature_tool,
                             sizeof(out_msg->feature_tool), false);
            if (rc == GW_OK) out_msg->has_feature_tool = 1;
            break;

        case GW_KEY_FEATURE_TOTAL: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT16_MAX, &value);
            if (rc == GW_OK) {
                out_msg->feature_total = (uint16_t)value;
                out_msg->has_feature_total = 1;
            }
            break;
        }

        case GW_KEY_FEATURE_DECIMALS: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT8_MAX, &value);
            if (rc == GW_OK) {
                out_msg->feature_decimals = (uint8_t)value;
                out_msg->has_feature_decimals = 1;
            }
            break;
        }

        /* Settings v2 fields (Phase 0 — additive, backward-compatible). */
        case GW_KEY_SETTINGS_SUPPORTED: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, 1u, &value);
            if (rc == GW_OK) {
                out_msg->settings_supported = (int)value;
                out_msg->has_settings_supported = 1;
            }
            break;
        }

        case GW_KEY_SETTINGS_SCHEMA_REVISION: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT16_MAX, &value);
            if (rc == GW_OK) {
                out_msg->settings_schema_revision = (uint16_t)value;
                out_msg->has_settings_schema_revision = 1;
            }
            break;
        }

        case GW_KEY_SETTINGS_ID:
            rc = gw_get_text(&r, out_msg->setting_id,
                             sizeof(out_msg->setting_id), false);
            if (rc == GW_OK) out_msg->has_setting_id = 1;
            break;

        case GW_KEY_SETTINGS_TITLE:
            rc = gw_get_text(&r, out_msg->setting_title,
                             sizeof(out_msg->setting_title), true);
            break;

        case GW_KEY_SETTINGS_GROUP:
            rc = gw_get_text(&r, out_msg->setting_group,
                             sizeof(out_msg->setting_group), true);
            break;

        case GW_KEY_SETTINGS_UNIT:
            rc = gw_get_text(&r, out_msg->setting_unit,
                             sizeof(out_msg->setting_unit), true);
            break;

        case GW_KEY_SETTINGS_TYPE: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT8_MAX, &value);
            if (rc == GW_OK) {
                out_msg->setting_type = (uint8_t)value;
                out_msg->has_setting_type = 1;
            }
            break;
        }

        case GW_KEY_SETTINGS_FLAGS: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT16_MAX, &value);
            if (rc == GW_OK) {
                out_msg->setting_flags = (uint16_t)value;
                out_msg->has_setting_flags = 1;
            }
            break;
        }

        case GW_KEY_SETTINGS_MAX_LENGTH: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT16_MAX, &value);
            if (rc == GW_OK) {
                out_msg->setting_max_length = (uint16_t)value;
                out_msg->has_setting_max_length = 1;
            }
            break;
        }

        case GW_KEY_SETTINGS_OPTION_COUNT: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT8_MAX, &value);
            if (rc == GW_OK) {
                out_msg->setting_option_count = (uint8_t)value;
                out_msg->has_setting_option_count = 1;
            }
            break;
        }

        case GW_KEY_SETTINGS_OPTION_INDEX: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT8_MAX, &value);
            if (rc == GW_OK) {
                out_msg->setting_option_index = (uint8_t)value;
                out_msg->has_setting_option_index = 1;
            }
            break;
        }

        case GW_KEY_SETTINGS_TRANSACTION_ID: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT64_MAX, &value);
            if (rc == GW_OK) {
                out_msg->settings_transaction_id = value;
                out_msg->has_settings_transaction_id = 1;
            }
            break;
        }

        case GW_KEY_SETTINGS_EXPECTED_REVISION: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT32_MAX, &value);
            if (rc == GW_OK) {
                out_msg->settings_expected_revision = (uint32_t)value;
                out_msg->has_settings_expected_revision = 1;
            }
            break;
        }

        case GW_KEY_SETTINGS_NEW_REVISION: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT32_MAX, &value);
            if (rc == GW_OK) {
                out_msg->settings_new_revision = (uint32_t)value;
                out_msg->has_settings_new_revision = 1;
            }
            break;
        }

        case GW_KEY_SETTINGS_VALUE: {
            size_t value_start = r.off;
            rc = gw_skip_item(&r, 0);
            if (rc != GW_OK) break;
            raw_setting_value_len = r.off - value_start;
            if (raw_setting_value_len > sizeof(raw_setting_value)) {
                rc = GW_ERR_DECODE;
                break;
            }
            memcpy(raw_setting_value, &r.data[value_start],
                   raw_setting_value_len);
            has_raw_setting_value = true;
            break;
        }

        case GW_KEY_SETTINGS_SEQUENCE: {
            uint64_t value = 0;
            rc = gw_get_uint_bounded(&r, UINT16_MAX, &value);
            if (rc == GW_OK) {
                out_msg->has_setting_id = 1;  /* Sequence used as item index */
            }
            break;
        }

        default:
            rc = gw_skip_item(&r, 0);
            break;
        }
    }

    if (rc != GW_OK) return rc;
    if (r.off != len) return GW_ERR_DECODE; /* trailing garbage */

    /* Required-field validation, mirroring Gateway decoder (#55). */
    if (!has_type || !has_command || !has_int_value || !has_bool_value) {
        return GW_ERR_DECODE;
    }
    /* Strict v4: version must be present and exact; v1/v2/v3 reject. */
    if (!has_version || version != (uint64_t)GW_PROTOCOL_VERSION) {
        GW_LOGE("decode: unsupported protocol_version %llu",
                (unsigned long long)version);
        return GW_ERR_UNSUPPORTED_VERSION;
    }
    if (has_ble_addr && !has_ble_addr_type) return GW_ERR_DECODE;
    if (has_raw_setting_value) {
        if (!out_msg->has_setting_type) return GW_ERR_DECODE;
        rc = gw_decode_setting_value(raw_setting_value, raw_setting_value_len,
                                     out_msg->setting_type, out_msg);
        if (rc != GW_OK) return rc;
    }

    out_msg->protocol_version = (uint8_t)version;
    out_msg->int_value = int_value;
    out_msg->bool_value = bool_value ? 1 : 0;
    out_msg->has_int_value = 1;
    out_msg->has_bool_value = 1;
    if (has_request_id) {
        out_msg->request_id = (uint32_t)request_id;
        out_msg->has_request_id = 1;
    }
    if (has_device_id) out_msg->has_device_id = 1;
    if (has_ble_addr) out_msg->has_ble_addr = 1;
    return GW_OK;
}

/* ================================================================== *
 * Builders + TX validation
 * ================================================================== */

void gw_build_ack(gw_message_t *ack, const gw_message_t *request,
                  const char *device_id, bool success, int int_value)
{
    gw_message_init(ack);
    if (request == NULL) return;

    gw_copy_str(ack->type, sizeof(ack->type), GW_MSG_TYPE_DEVICE_ACK);
    gw_copy_str(ack->command, sizeof(ack->command), request->command);

    /* Exact echo rules: request_id copied verbatim, never regenerated. */
    if (request->has_request_id && request->request_id != 0u) {
        ack->request_id = request->request_id;
        ack->has_request_id = 1;
    }

    /* Logical identity: explicit argument wins over embedded request one. */
    const char *logical_id =
        (device_id != NULL && device_id[0] != '\0') ? device_id
                                                    : request->device_id;
    if (logical_id[0] != '\0') {
        gw_copy_str(ack->device_id, sizeof(ack->device_id), logical_id);
        ack->has_device_id = 1;
    }

    ack->int_value = int_value;
    ack->bool_value = success ? 1 : 0;
    ack->has_int_value = 1;
    ack->has_bool_value = 1;
    /* Strict v4: ACKs always advertise the current protocol version. */
}

void gw_build_event(gw_message_t *event, const char *device_id,
                    const char *event_name, int int_value, bool bool_value)
{
    gw_message_init(event);
    gw_copy_str(event->type, sizeof(event->type), GW_MSG_TYPE_DEVICE_EVENT);
    gw_copy_str(event->command, sizeof(event->command),
                event_name != NULL ? event_name : "");
    if (device_id != NULL && device_id[0] != '\0') {
        gw_copy_str(event->device_id, sizeof(event->device_id), device_id);
        event->has_device_id = 1;
    }
    event->int_value = int_value;
    event->bool_value = bool_value ? 1 : 0;
    event->has_int_value = 1;
    event->has_bool_value = 1;
}

void gw_build_feature_event_bool(gw_message_t *event, const char *device_id,
                                 const char *feature_id, uint8_t property_id,
                                 bool value)
{
    gw_build_event(event, device_id, GW_EVENT_FEATURE_STATE, 0, false);
    if (feature_id == NULL || feature_id[0] == '\0') return;
    gw_copy_str(event->feature_id, sizeof(event->feature_id), feature_id);
    event->has_feature_id = 1;
    event->property_id = property_id;
    event->has_property_id = 1;
    event->feature_value_bool = value;
    event->has_feature_value_bool = 1;
}

void gw_build_feature_event_int(gw_message_t *event, const char *device_id,
                                const char *feature_id, uint8_t property_id,
                                int32_t value)
{
    gw_build_event(event, device_id, GW_EVENT_FEATURE_STATE, value, false);
    if (feature_id == NULL || feature_id[0] == '\0') return;
    gw_copy_str(event->feature_id, sizeof(event->feature_id), feature_id);
    event->has_feature_id = 1;
    event->property_id = property_id;
    event->has_property_id = 1;
    event->feature_value_int = value;
    event->has_feature_value_int = 1;
}

static bool gw_non_empty(const char *value, size_t capacity)
{
    return value[0] != '\0' && strnlen(value, capacity) < capacity;
}

bool gw_message_valid_ack(const gw_message_t *msg)
{
    if (msg == NULL) return false;
    if (strcmp(msg->type, GW_MSG_TYPE_DEVICE_ACK) != 0) return false;
    if (!gw_non_empty(msg->device_id, sizeof(msg->device_id))) return false;
    if (!gw_non_empty(msg->command, sizeof(msg->command))) return false;
    if (!msg->has_request_id || msg->request_id == 0u) return false;
    return true;
}

bool gw_message_valid_event(const gw_message_t *msg)
{
    if (msg == NULL) return false;
    if (strcmp(msg->type, GW_MSG_TYPE_DEVICE_EVENT) != 0) return false;
    if (!gw_non_empty(msg->command, sizeof(msg->command))) return false;
    return true;
}
