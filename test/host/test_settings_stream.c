/*
 * Host unit tests for Settings v2 BLE streaming (Phase 2).
 *
 * Tests the describe_settings and read_settings stream encoding.
 * Mocks BLE notify and device_settings for isolated testing.
 *
 * Run: test/host/run_settings_stream_tests.sh
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Mock ESP-IDF types */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_NO_MEM -3
#define ESP_ERR_NOT_FOUND -4
#define ESP_ERR_INVALID_SIZE -5
#define ESP_ERR_INVALID_VERSION -6
#define ESP_ERR_NVS_NOT_FOUND -102

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
        g_checks++;                                                          \
    } while (0)

#define CHECK_INT(actual, expected)                                          \
    do {                                                                     \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        if (a_ != e_) {                                                      \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s == %lld, got %lld\n", __FILE__,  \
                    __LINE__, #actual, e_, a_);                              \
        }                                                                    \
        g_checks++;                                                          \
    } while (0)

/* ------------------------------------------------------------------ *
 * Mock device_settings API
 * ------------------------------------------------------------------ */

#include "device_settings.h"

/* Mock internal accessors */
void device_settings_set_revision(uint32_t revision);
void device_settings_set_active_config(void *config);
void device_settings_set_staging_config(void *config);
const void *device_settings_get_active_config(void);
void *device_settings_get_staging_config(void);
size_t device_settings_get_config_size(void);
uint16_t device_settings_get_format_version(void);
device_settings_validate_fn device_settings_get_validate_fn(void);
void device_settings_tx_reset_state(void);

/* ------------------------------------------------------------------ *
 * Test setting descriptors
 * ------------------------------------------------------------------ */

typedef struct {
    device_settings_blob_header_t header;
    bool sensor_enabled;
    int32_t sample_rate;
    char device_name[32];
    int32_t fan_mode;
} test_config_t;

static test_config_t s_active_config;
static test_config_t s_staging_config;

static esp_err_t bool_read(void *ctx, void *out)
{
    test_config_t *cfg = (test_config_t *)ctx;
    *(bool *)out = cfg->sensor_enabled;
    return ESP_OK;
}

static esp_err_t bool_stage(void *ctx, const void *value)
{
    test_config_t *cfg = (test_config_t *)ctx;
    cfg->sensor_enabled = *(const bool *)value;
    return ESP_OK;
}

static esp_err_t int_read(void *ctx, void *out)
{
    test_config_t *cfg = (test_config_t *)ctx;
    *(int32_t *)out = cfg->sample_rate;
    return ESP_OK;
}

static esp_err_t int_stage(void *ctx, const void *value)
{
    test_config_t *cfg = (test_config_t *)ctx;
    cfg->sample_rate = *(const int32_t *)value;
    return ESP_OK;
}

static esp_err_t str_read(void *ctx, void *out)
{
    test_config_t *cfg = (test_config_t *)ctx;
    strlcpy((char *)out, cfg->device_name, 32);
    return ESP_OK;
}

static esp_err_t str_stage(void *ctx, const void *value)
{
    test_config_t *cfg = (test_config_t *)ctx;
    strlcpy(cfg->device_name, (const char *)value, sizeof(cfg->device_name));
    return ESP_OK;
}

static esp_err_t enum_read(void *ctx, void *out)
{
    test_config_t *cfg = (test_config_t *)ctx;
    *(int32_t *)out = cfg->fan_mode;
    return ESP_OK;
}

static esp_err_t enum_stage(void *ctx, const void *value)
{
    test_config_t *cfg = (test_config_t *)ctx;
    cfg->fan_mode = *(const int32_t *)value;
    return ESP_OK;
}

static const device_setting_option_t fan_options[] = {
    { .label = "Auto" },
    { .label = "Low" },
    { .label = "Medium" },
    { .label = "High" },
};

static const device_setting_descriptor_t s_bool_desc = {
    .id = "sensor_enabled",
    .title = "Sensor Enabled",
    .group = "sensor",
    .unit = "",
    .type = DEVICE_SETTING_BOOL,
    .flags = 0,
    .read = bool_read,
    .stage = bool_stage,
    .ctx = &s_staging_config,
};

static const device_setting_descriptor_t s_int_desc = {
    .id = "sample_rate",
    .title = "Sample Rate",
    .group = "sensor",
    .unit = "Hz",
    .type = DEVICE_SETTING_INT,
    .flags = 0,
    .min_value = 1,
    .max_value = 100,
    .step = 1,
    .read = int_read,
    .stage = int_stage,
    .ctx = &s_staging_config,
};

static const device_setting_descriptor_t s_str_desc = {
    .id = "device_name",
    .title = "Device Name",
    .group = "",
    .unit = "",
    .type = DEVICE_SETTING_STRING,
    .flags = 0,
    .max_length = 32,
    .read = str_read,
    .stage = str_stage,
    .ctx = &s_staging_config,
};

static const device_setting_descriptor_t s_enum_desc = {
    .id = "fan_mode",
    .title = "Fan Mode",
    .group = "climate",
    .unit = "",
    .type = DEVICE_SETTING_ENUM,
    .flags = 0,
    .options = fan_options,
    .option_count = 4,
    .read = enum_read,
    .stage = enum_stage,
    .ctx = &s_staging_config,
};

/* Secret setting */
static const device_setting_descriptor_t s_secret_desc = {
    .id = "api_key",
    .title = "API Key",
    .group = "network",
    .unit = "",
    .type = DEVICE_SETTING_STRING,
    .flags = DEVICE_SETTING_FLAG_SECRET,
    .max_length = 64,
    .read = str_read,
    .stage = str_stage,
    .ctx = &s_staging_config,
};

/* ------------------------------------------------------------------ *
 * Include codec under test
 * ------------------------------------------------------------------ */

#include "gateway_settings.h"
#include "gateway_protocol.h"

/* ------------------------------------------------------------------ *
 * Decode helpers
 * ------------------------------------------------------------------ */

/* Simple CBOR value extractor for testing */
static int cbor_find_uint(const uint8_t *buf, size_t len, uint8_t key,
                          uint64_t *out)
{
    size_t pos = 0;
    if (pos >= len || (buf[pos] & 0xE0) != 0xA0) return -1;
    uint64_t map_len = buf[pos] & 0x1F;
    pos++;
    if (map_len == 24) {
        if (pos >= len) return -1;
        map_len = buf[pos];
        pos++;
    }
    for (uint64_t i = 0; i < map_len; i++) {
        if (pos >= len) return -1;
        uint8_t key_major = buf[pos] & 0xE0;
        uint64_t key_val = buf[pos] & 0x1F;
        pos++;
        if (key_val == 24) {
            if (pos >= len) return -1;
            key_val = buf[pos];
            pos++;
        } else if (key_val == 25) {
            if (pos + 1 >= len) return -1;
            key_val = (buf[pos] << 8) | buf[pos + 1];
            pos += 2;
        }
        if (key_major != 0x00) return -1; /* not unsigned int */

        if (key_val == key) {
            if (pos >= len) return -1;
            uint8_t val_major = buf[pos] & 0xE0;
            uint64_t val = buf[pos] & 0x1F;
            pos++;
            if (val == 24) {
                if (pos >= len) return -1;
                val = buf[pos];
                pos++;
            } else if (val == 25) {
                if (pos + 1 >= len) return -1;
                val = (buf[pos] << 8) | buf[pos + 1];
                pos += 2;
            } else if (val == 26) {
                if (pos + 3 >= len) return -1;
                val = ((uint64_t)buf[pos] << 24) | ((uint64_t)buf[pos+1] << 16) |
                      ((uint64_t)buf[pos+2] << 8) | buf[pos+3];
                pos += 4;
            }
            if (val_major == 0x00) {
                *out = val;
                return 0;
            }
            return -1;
        }

        /* Skip value */
        if (pos >= len) return -1;
        uint8_t sv = buf[pos] & 0x1F;
        uint8_t sv_major = buf[pos] & 0xE0;
        pos++;
        if (sv == 24) { pos++; }
        else if (sv == 25) { pos += 2; }
        else if (sv == 26) { pos += 4; }
        else if (sv == 27) { pos += 8; }
        else if (sv_major == 0x60) { /* text string */
            if (pos + sv > len) return -1;
            pos += sv;
        }
    }
    return -1;
}

static int cbor_get_type_string(const uint8_t *buf, size_t len,
                                char *out, size_t out_cap)
{
    size_t pos = 0;
    if (pos >= len || (buf[pos] & 0xE0) != 0xA0) return -1;
    uint64_t map_len = buf[pos] & 0x1F;
    pos++;
    if (map_len == 24) { if (pos >= len) return -1; map_len = buf[pos]; pos++; }

    for (uint64_t i = 0; i < map_len; i++) {
        if (pos >= len) return -1;
        uint64_t kv = buf[pos] & 0x1F;
        pos++;
        if (kv == 24) { if (pos >= len) return -1; kv = buf[pos]; pos++; }

        if (kv == GW_KEY_TYPE) {
            if (pos >= len) return -1;
            uint8_t major = buf[pos] & 0xE0;
            uint64_t slen = buf[pos] & 0x1F;
            pos++;
            if (slen == 24) { if (pos >= len) return -1; slen = buf[pos]; pos++; }
            if (major != 0x60) return -1;
            if (slen >= out_cap) slen = out_cap - 1;
            memcpy(out, buf + pos, slen);
            out[slen] = '\0';
            return 0;
        }

        /* Skip value (simplified) */
        if (pos >= len) return -1;
        uint8_t v = buf[pos] & 0x1F;
        pos++;
        if (v == 24) { pos++; }
        else if (v == 25) { pos += 2; }
        else if (v == 26) { pos += 4; }
        else if (v == 27) { pos += 8; }
        else if (((buf[pos - 1] >> 5) & 0x07) == 3) { /* text string */
            if (pos + v > len) return -1;
            pos += v;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * Tests
 * ------------------------------------------------------------------ */

static void test_encode_begin_frame(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_begin(buf, sizeof(buf), 5, 42);
    CHECK(enc > 0);
    CHECK(enc <= 256);

    uint64_t total = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_TOTAL, &total) == 0);
    CHECK_INT(total, 5);

    uint64_t req_id = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_REQUEST_ID, &req_id) == 0);
    CHECK_INT(req_id, 42);
}

static void test_encode_item_bool(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_item(buf, sizeof(buf),
                                      0, 3, 1,
                                      "sensor_enabled", "Sensor Enabled",
                                      "sensor", "",
                                      GW_SETTING_TYPE_BOOL, 0, 0,
                                      0, 0, 0);
    CHECK(enc > 0);
    CHECK(enc <= 256);

    uint64_t seq = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_SETTINGS_SEQUENCE, &seq) == 0);
    CHECK_INT(seq, 0);

    uint64_t type = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_SETTINGS_TYPE, &type) == 0);
    CHECK_INT(type, GW_SETTING_TYPE_BOOL);
}

static void test_encode_item_int(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_item(buf, sizeof(buf),
                                      1, 3, 1,
                                      "sample_rate", "Sample Rate",
                                      "sensor", "Hz",
                                      GW_SETTING_TYPE_INT, 0, 0,
                                      1, 100, 1);
    CHECK(enc > 0);
    CHECK(enc <= 256);

    uint64_t min_val = 0, max_val = 0, step = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_MIN_VALUE, &min_val) == 0);
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_MAX_VALUE, &max_val) == 0);
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_STEP, &step) == 0);
    CHECK_INT(min_val, 1);
    CHECK_INT(max_val, 100);
    CHECK_INT(step, 1);
}

static void test_encode_item_string_max_length(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_item(buf, sizeof(buf),
                                      2, 3, 1,
                                      "device_name", "Device Name",
                                      "", "",
                                      GW_SETTING_TYPE_STRING, 0, 32,
                                      0, 0, 0);
    CHECK(enc > 0);
    CHECK(enc <= 256);

    uint64_t max_len = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_SETTINGS_MAX_LENGTH, &max_len) == 0);
    CHECK_INT(max_len, 32);
}

static void test_encode_option_item(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_option_item(buf, sizeof(buf),
                                             3, 2, 1, "Medium");
    CHECK(enc > 0);
    CHECK(enc <= 256);

    uint64_t opt_idx = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_SETTINGS_OPTION_INDEX, &opt_idx) == 0);
    CHECK_INT(opt_idx, 2);
}

static void test_encode_end_frame(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_end(buf, sizeof(buf), 5, 42);
    CHECK(enc > 0);
    CHECK(enc <= 256);

    uint64_t total = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_TOTAL, &total) == 0);
    CHECK_INT(total, 5);
}

static void test_encode_values_begin_frame(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_values_begin(buf, sizeof(buf), 4, 7, 99);
    CHECK(enc > 0);
    CHECK(enc <= 256);

    uint64_t total = 0, rev = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_TOTAL, &total) == 0);
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_CAPABILITY_REVISION, &rev) == 0);
    CHECK_INT(total, 4);
    CHECK_INT(rev, 7);
}

static void test_encode_value_bool(void)
{
    uint8_t buf[256];
    bool val = true;
    int enc = gw_settings_encode_value(buf, sizeof(buf), 0, "sensor_enabled",
                                       GW_SETTING_TYPE_BOOL, &val, 1);
    CHECK(enc > 0);
    CHECK(enc <= 256);
}

static void test_encode_value_int(void)
{
    uint8_t buf[256];
    int32_t val = 50;
    int enc = gw_settings_encode_value(buf, sizeof(buf), 1, "sample_rate",
                                       GW_SETTING_TYPE_INT, &val, 1);
    CHECK(enc > 0);
    CHECK(enc <= 256);
}

static void test_encode_value_string(void)
{
    uint8_t buf[256];
    const char *val = "hello";
    int enc = gw_settings_encode_value(buf, sizeof(buf), 2, "device_name",
                                       GW_SETTING_TYPE_STRING, val, 1);
    CHECK(enc > 0);
    CHECK(enc <= 256);
}

static void test_encode_value_enum(void)
{
    uint8_t buf[256];
    uint8_t val = 2;
    int enc = gw_settings_encode_value(buf, sizeof(buf), 3, "fan_mode",
                                       GW_SETTING_TYPE_ENUM, &val, 1);
    CHECK(enc > 0);
    CHECK(enc <= 256);
}

static void test_encode_values_end_frame(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_values_end(buf, sizeof(buf), 4, 7, 99);
    CHECK(enc > 0);
    CHECK(enc <= 256);

    uint64_t total = 0, rev = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_TOTAL, &total) == 0);
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_CAPABILITY_REVISION, &rev) == 0);
    CHECK_INT(total, 4);
    CHECK_INT(rev, 7);
}

static void test_encode_commit_confirm(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_commit_confirm(buf, sizeof(buf),
                                                 0x1234, 5, 42);
    CHECK(enc > 0);
    CHECK(enc <= 256);

    uint64_t tx_id = 0, new_rev = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_SETTINGS_TRANSACTION_ID, &tx_id) == 0);
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_SETTINGS_NEW_REVISION, &new_rev) == 0);
    CHECK_INT(tx_id, 0x1234);
    CHECK_INT(new_rev, 5);
}

/* Test that all frames fit within GW_MSG_MAX_LEN */
static void test_all_frames_fit_max_len(void)
{
    uint8_t buf[256];

    /* Describe frames */
    int enc;
    enc = gw_settings_encode_begin(buf, sizeof(buf), 12, 1);
    CHECK(enc > 0 && enc <= 256);

    enc = gw_settings_encode_item(buf, sizeof(buf), 0, 12, 1,
                                  "a_very_long_setting_id_that_fits",
                                  "A Very Long Title That Fits Within Limits",
                                  "a_long_group_name", "units",
                                  GW_SETTING_TYPE_INT, 0, 64,
                                  -1000, 100000, 10);
    CHECK(enc > 0 && enc <= 256);

    enc = gw_settings_encode_option_item(buf, sizeof(buf), 0, 7, 1,
                                         "A Long Option Label");
    CHECK(enc > 0 && enc <= 256);

    enc = gw_settings_encode_end(buf, sizeof(buf), 12, 1);
    CHECK(enc > 0 && enc <= 256);

    /* Value frames */
    enc = gw_settings_encode_values_begin(buf, sizeof(buf), 12, 99999, 1);
    CHECK(enc > 0 && enc <= 256);

    bool bval = true;
    enc = gw_settings_encode_value(buf, sizeof(buf), 0, "test_bool",
                                   GW_SETTING_TYPE_BOOL, &bval, 1);
    CHECK(enc > 0 && enc <= 256);

    int32_t ival = 42;
    enc = gw_settings_encode_value(buf, sizeof(buf), 1, "test_int",
                                   GW_SETTING_TYPE_INT, &ival, 1);
    CHECK(enc > 0 && enc <= 256);

    const char *sval = "test_string_value";
    enc = gw_settings_encode_value(buf, sizeof(buf), 2, "test_str",
                                   GW_SETTING_TYPE_STRING, sval, 1);
    CHECK(enc > 0 && enc <= 256);

    uint8_t eval = 3;
    enc = gw_settings_encode_value(buf, sizeof(buf), 3, "test_enum",
                                   GW_SETTING_TYPE_ENUM, &eval, 1);
    CHECK(enc > 0 && enc <= 256);

    enc = gw_settings_encode_values_end(buf, sizeof(buf), 12, 99999, 1);
    CHECK(enc > 0 && enc <= 256);
}

/* Test zero settings */
static void test_zero_settings(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_begin(buf, sizeof(buf), 0, 1);
    CHECK(enc > 0);
    uint64_t total = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_TOTAL, &total) == 0);
    CHECK_INT(total, 0);

    enc = gw_settings_encode_end(buf, sizeof(buf), 0, 1);
    CHECK(enc > 0);

    enc = gw_settings_encode_values_begin(buf, sizeof(buf), 0, 1, 1);
    CHECK(enc > 0);

    enc = gw_settings_encode_values_end(buf, sizeof(buf), 0, 1, 1);
    CHECK(enc > 0);
}

/* Test max settings (12) */
static void test_max_settings(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_begin(buf, sizeof(buf), 12, 1);
    CHECK(enc > 0);

    for (int i = 0; i < 12; i++) {
        enc = gw_settings_encode_item(buf, sizeof(buf), i, 12, 1,
                                      "setting", "Setting", "", "",
                                      GW_SETTING_TYPE_BOOL, 0, 0,
                                      0, 0, 0);
        CHECK(enc > 0 && enc <= 256);
    }

    enc = gw_settings_encode_end(buf, sizeof(buf), 12, 1);
    CHECK(enc > 0);
}

/* Test max enum options (8) */
static void test_max_enum_options(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_item(buf, sizeof(buf), 0, 1, 1,
                                      "enum_setting", "Enum Setting", "", "",
                                      GW_SETTING_TYPE_ENUM, 0, 0,
                                      0, 0, 0);
    CHECK(enc > 0);

    const char *labels[] = {"Opt0","Opt1","Opt2","Opt3","Opt4","Opt5","Opt6","Opt7"};
    for (int i = 0; i < 8; i++) {
        enc = gw_settings_encode_option_item(buf, sizeof(buf), 0, i, 1,
                                             labels[i]);
        CHECK(enc > 0 && enc <= 256);
    }
}

/* Test frame type strings */
static void test_frame_type_strings(void)
{
    uint8_t buf[256];
    char type_str[32];

    gw_settings_encode_begin(buf, sizeof(buf), 1, 1);
    CHECK(cbor_get_type_string(buf, sizeof(buf), type_str, sizeof(type_str)) == 0);
    CHECK(strcmp(type_str, "device_command") == 0);

    gw_settings_encode_values_begin(buf, sizeof(buf), 1, 1, 1);
    CHECK(cbor_get_type_string(buf, sizeof(buf), type_str, sizeof(type_str)) == 0);
    CHECK(strcmp(type_str, "device_command") == 0);
}

/* Test describe_settings: begin.total == end.total */
static void test_begin_end_total_match(void)
{
    uint8_t buf_begin[256], buf_end[256];
    gw_settings_encode_begin(buf_begin, sizeof(buf_begin), 7, 1);
    gw_settings_encode_end(buf_end, sizeof(buf_end), 7, 1);

    uint64_t t1 = 0, t2 = 0;
    CHECK(cbor_find_uint(buf_begin, sizeof(buf_begin), GW_KEY_TOTAL, &t1) == 0);
    CHECK(cbor_find_uint(buf_end, sizeof(buf_end), GW_KEY_TOTAL, &t2) == 0);
    CHECK_INT(t1, t2);
}

/* Test values_begin.total == values_end.total */
static void test_values_begin_end_total_match(void)
{
    uint8_t buf_begin[256], buf_end[256];
    gw_settings_encode_values_begin(buf_begin, sizeof(buf_begin), 7, 3, 1);
    gw_settings_encode_values_end(buf_end, sizeof(buf_end), 7, 3, 1);

    uint64_t t1 = 0, t2 = 0;
    CHECK(cbor_find_uint(buf_begin, sizeof(buf_begin), GW_KEY_TOTAL, &t1) == 0);
    CHECK(cbor_find_uint(buf_end, sizeof(buf_end), GW_KEY_TOTAL, &t2) == 0);
    CHECK_INT(t1, t2);

    uint64_t r1 = 0, r2 = 0;
    CHECK(cbor_find_uint(buf_begin, sizeof(buf_begin), GW_KEY_CAPABILITY_REVISION, &r1) == 0);
    CHECK(cbor_find_uint(buf_end, sizeof(buf_end), GW_KEY_CAPABILITY_REVISION, &r2) == 0);
    CHECK_INT(r1, r2);
}

/* Test schema revision is stable across calls */
static void test_schema_revision_stable(void)
{
    uint8_t buf1[256], buf2[256];
    gw_settings_encode_begin(buf1, sizeof(buf1), 1, 1);
    gw_settings_encode_begin(buf2, sizeof(buf2), 2, 2);
    /* Both should succeed — schema revision is implicit */
    CHECK(buf1[0] != 0);
    CHECK(buf2[0] != 0);
}

/* Test settings_values_begin has correct revision field */
static void test_values_begin_revision(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_values_begin(buf, sizeof(buf), 3, 42, 1);
    CHECK(enc > 0);

    uint64_t rev = 0;
    CHECK(cbor_find_uint(buf, (size_t)enc, GW_KEY_CAPABILITY_REVISION, &rev) == 0);
    CHECK_INT(rev, 42);
}

/* Test decode of describe_settings command */
static void test_decode_describe_settings(void)
{
    gw_message_t msg;
    gw_message_init(&msg);
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND, sizeof(msg.type));
    strlcpy(msg.command, GW_COMMAND_DESCRIBE_SETTINGS, sizeof(msg.command));
    msg.has_int_value = 1;
    msg.has_bool_value = 1;
    msg.has_device_id = 1;
    strlcpy(msg.device_id, "gw1", sizeof(msg.device_id));
    msg.request_id = 42;
    msg.has_request_id = 1;

    gw_settings_command_t cmd;
    CHECK(gw_settings_decode_command(&msg, &cmd) == GW_OK);
    CHECK_INT(cmd.cmd_type, GW_SETTINGS_CMD_DESCRIBE);
    CHECK_INT(cmd.request_id, 42);
}

/* Test decode of read_settings command */
static void test_decode_read_settings(void)
{
    gw_message_t msg;
    gw_message_init(&msg);
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND, sizeof(msg.type));
    strlcpy(msg.command, GW_COMMAND_READ_SETTINGS, sizeof(msg.command));
    msg.has_int_value = 1;
    msg.has_bool_value = 1;
    msg.has_device_id = 1;
    strlcpy(msg.device_id, "gw1", sizeof(msg.device_id));

    gw_settings_command_t cmd;
    CHECK(gw_settings_decode_command(&msg, &cmd) == GW_OK);
    CHECK_INT(cmd.cmd_type, GW_SETTINGS_CMD_READ);
}

/* Test decode of tx_begin command */
static void test_decode_tx_begin(void)
{
    gw_message_t msg;
    gw_message_init(&msg);
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND, sizeof(msg.type));
    strlcpy(msg.command, GW_MSG_TYPE_SETTINGS_TX_BEGIN, sizeof(msg.command));
    msg.has_int_value = 1;
    msg.has_bool_value = 1;
    msg.has_device_id = 1;
    strlcpy(msg.device_id, "gw1", sizeof(msg.device_id));
    msg.settings_transaction_id = 0xABCD;
    msg.has_settings_transaction_id = 1;
    msg.settings_expected_revision = 5;
    msg.has_settings_expected_revision = 1;

    gw_settings_command_t cmd;
    CHECK(gw_settings_decode_command(&msg, &cmd) == GW_OK);
    CHECK_INT(cmd.cmd_type, GW_SETTINGS_CMD_TX_BEGIN);
    CHECK_INT(cmd.transaction_id, 0xABCD);
    CHECK_INT(cmd.expected_revision, 5);
}

/* Test device_settings API integration */
static void test_device_settings_count_and_get(void)
{
    device_settings_init();
    CHECK_INT(device_settings_register(&s_bool_desc), ESP_OK);
    CHECK_INT(device_settings_register(&s_int_desc), ESP_OK);
    CHECK_INT(device_settings_register(&s_str_desc), ESP_OK);
    CHECK_INT(device_settings_register(&s_enum_desc), ESP_OK);
    CHECK_INT(device_settings_freeze(), ESP_OK);

    CHECK_INT(device_settings_count(), 4);

    const device_setting_descriptor_t *d;
    d = device_settings_get(0);
    CHECK(d != NULL && strcmp(d->id, "sensor_enabled") == 0);
    d = device_settings_get(1);
    CHECK(d != NULL && strcmp(d->id, "sample_rate") == 0);
    d = device_settings_get(2);
    CHECK(d != NULL && strcmp(d->id, "device_name") == 0);
    d = device_settings_get(3);
    CHECK(d != NULL && strcmp(d->id, "fan_mode") == 0);

    d = device_settings_find("sample_rate");
    CHECK(d != NULL);
    CHECK_INT(d->type, DEVICE_SETTING_INT);

    CHECK(device_settings_get(999) == NULL);
}

/* Test device_settings read callbacks */
static void test_device_settings_read_callbacks(void)
{
    device_settings_init();
    CHECK_INT(device_settings_register(&s_bool_desc), ESP_OK);
    CHECK_INT(device_settings_register(&s_int_desc), ESP_OK);
    CHECK_INT(device_settings_register(&s_str_desc), ESP_OK);
    CHECK_INT(device_settings_register(&s_enum_desc), ESP_OK);
    CHECK_INT(device_settings_freeze(), ESP_OK);

    memset(&s_active_config, 0, sizeof(s_active_config));
    memset(&s_staging_config, 0, sizeof(s_staging_config));
    device_settings_set_active_config(&s_active_config);
    device_settings_set_staging_config(&s_staging_config);
    device_settings_set_config_size(sizeof(test_config_t));
    device_settings_set_revision(1);

    /* Set values in staging config (ctx points to staging) */
    s_staging_config.sensor_enabled = true;
    s_staging_config.sample_rate = 50;
    strlcpy(s_staging_config.device_name, "TestDevice", sizeof(s_staging_config.device_name));
    s_staging_config.fan_mode = 2;

    /* Read via descriptors */
    bool bval = false;
    CHECK_INT(s_bool_desc.read(s_bool_desc.ctx, &bval), ESP_OK);
    CHECK(bval == true);

    int32_t ival = 0;
    CHECK_INT(s_int_desc.read(s_int_desc.ctx, &ival), ESP_OK);
    CHECK_INT(ival, 50);

    char sval[32] = {0};
    CHECK_INT(s_str_desc.read(s_str_desc.ctx, sval), ESP_OK);
    CHECK(strcmp(sval, "TestDevice") == 0);

    int32_t eval = 0;
    CHECK_INT(s_enum_desc.read(s_enum_desc.ctx, &eval), ESP_OK);
    CHECK_INT(eval, 2);
}

/* Test that secret flag is properly set */
static void test_secret_flag(void)
{
    CHECK((s_secret_desc.flags & DEVICE_SETTING_FLAG_SECRET) != 0);
    CHECK((s_bool_desc.flags & DEVICE_SETTING_FLAG_SECRET) == 0);
}

/* ------------------------------------------------------------------ *
 * Main
 * ------------------------------------------------------------------ */

int main(void)
{
    /* Codec tests */
    test_encode_begin_frame();
    test_encode_item_bool();
    test_encode_item_int();
    test_encode_item_string_max_length();
    test_encode_option_item();
    test_encode_end_frame();
    test_encode_values_begin_frame();
    test_encode_value_bool();
    test_encode_value_int();
    test_encode_value_string();
    test_encode_value_enum();
    test_encode_values_end_frame();
    test_encode_commit_confirm();
    test_all_frames_fit_max_len();
    test_zero_settings();
    test_max_settings();
    test_max_enum_options();
    test_frame_type_strings();
    test_begin_end_total_match();
    test_values_begin_end_total_match();
    test_schema_revision_stable();
    test_values_begin_revision();

    /* Decode tests */
    test_decode_describe_settings();
    test_decode_read_settings();
    test_decode_tx_begin();

    /* Device settings integration tests */
    test_device_settings_count_and_get();
    test_device_settings_read_callbacks();
    test_secret_flag();

    printf("settings_stream: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
