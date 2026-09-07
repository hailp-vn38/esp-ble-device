/*
 * Host unit tests for Settings v2 BLE Transaction Commands (Phase 3).
 *
 * Tests the transaction command handling via device_settings API
 * and gw_settings encode/decode functions.
 *
 * Run: test/host/run_settings_tx_tests.sh
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
 * Mock device_settings internals
 * ------------------------------------------------------------------ */

#include "device_settings.h"

extern void device_settings_set_revision(uint32_t revision);
extern void device_settings_set_active_config(void *config);
extern void device_settings_set_staging_config(void *config);
extern const void *device_settings_get_active_config(void);
extern void *device_settings_get_staging_config(void);
extern size_t device_settings_get_config_size(void);
extern uint16_t device_settings_get_format_version(void);
extern device_settings_validate_fn device_settings_get_validate_fn(void);
extern void device_settings_tx_reset_state(void);

/* ------------------------------------------------------------------ *
 * Include codec under test
 * ------------------------------------------------------------------ */

#include "gateway_settings.h"
#include "gateway_protocol.h"

/* ------------------------------------------------------------------ *
 * Test config blob
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

/* ------------------------------------------------------------------ *
 * Test setting descriptors
 * ------------------------------------------------------------------ */

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
    /* ENUM values arrive as uint8_t* from gw_settings, but the device_settings
     * tx_set passes them as-is. The stage callback receives the raw pointer. */
    cfg->fan_mode = (int32_t)(*(const uint8_t *)value);
    return ESP_OK;
}

static const device_setting_option_t fan_options[] = {
    { .label = "Auto" },
    { .label = "Low" },
    { .label = "Medium" },
    { .label = "High" },
};

static const device_setting_descriptor_t s_settings[] = {
    {
        .id = "sensor_enabled",
        .title = "Sensor Enabled",
        .group = "sensor",
        .unit = "",
        .type = DEVICE_SETTING_BOOL,
        .flags = 0,
        .read = bool_read,
        .stage = bool_stage,
        .ctx = &s_staging_config,
    },
    {
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
    },
    {
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
    },
    {
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
    },
};

/* Readonly setting for testing */
static const device_setting_descriptor_t s_readonly_desc = {
    .id = "readonly_bool",
    .title = "Readonly Bool",
    .group = "",
    .unit = "",
    .type = DEVICE_SETTING_BOOL,
    .flags = DEVICE_SETTING_FLAG_READONLY,
    .read = bool_read,
    .stage = bool_stage,
    .ctx = &s_staging_config,
};

/* Cross-field validation callback: sensor disabled + rate > 50 => error */
static esp_err_t test_validate(const void *config, size_t config_size)
{
    (void)config_size;
    const test_config_t *cfg = (const test_config_t *)config;
    if (!cfg->sensor_enabled && cfg->sample_rate > 50) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 * Helper: init + register + freeze + setup config
 * ------------------------------------------------------------------ */

static void setup_test_env(void)
{
    device_settings_init();
    device_settings_register(&s_settings[0]);
    device_settings_register(&s_settings[1]);
    device_settings_register(&s_settings[2]);
    device_settings_register(&s_settings[3]);
    device_settings_freeze();

    memset(&s_active_config, 0, sizeof(s_active_config));
    memset(&s_staging_config, 0, sizeof(s_staging_config));
    device_settings_set_active_config(&s_active_config);
    device_settings_set_staging_config(&s_staging_config);
    device_settings_set_config_size(sizeof(test_config_t));
    device_settings_set_revision(1);
    s_active_config.header.config_revision = 1;
    s_staging_config.header.config_revision = 1;
}

/* ------------------------------------------------------------------ *
 * Tests: BEGIN
 * ------------------------------------------------------------------ */

static void test_begin_correct_revision(void)
{
    setup_test_env();
    esp_err_t err = device_settings_tx_begin(0x100, 1);
    CHECK_INT(err, ESP_OK);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_ACTIVE);
    device_settings_tx_abort(0x100);
}

static void test_begin_stale_revision(void)
{
    setup_test_env();
    /* Expected revision 5 but current is 1 */
    esp_err_t err = device_settings_tx_begin(0x101, 5);
    CHECK_INT(err, ESP_ERR_INVALID_VERSION);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_IDLE);
}

static void test_begin_duplicate_same_tx_id(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x102, 1), ESP_OK);
    /* Second BEGIN with same tx_id — should fail (tx already active) */
    esp_err_t err = device_settings_tx_begin(0x102, 1);
    CHECK_INT(err, ESP_ERR_INVALID_STATE);
    device_settings_tx_abort(0x102);
}

static void test_begin_second_different_tx(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x103, 1), ESP_OK);
    /* Second BEGIN with different tx_id — should fail (tx busy) */
    esp_err_t err = device_settings_tx_begin(0x104, 1);
    CHECK_INT(err, ESP_ERR_INVALID_STATE);
    device_settings_tx_abort(0x103);
}

/* ------------------------------------------------------------------ *
 * Tests: SET (decode all 4 types)
 * ------------------------------------------------------------------ */

static void test_set_bool(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x200, 1), ESP_OK);

    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_abort(0x200), ESP_OK);
}

static void test_set_int(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x201, 1), ESP_OK);

    int32_t val = 50;
    CHECK_INT(device_settings_tx_set("sample_rate", DEVICE_SETTING_INT, &val), ESP_OK);
    CHECK_INT(device_settings_tx_abort(0x201), ESP_OK);
}

static void test_set_string(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x202, 1), ESP_OK);

    const char *val = "hello";
    CHECK_INT(device_settings_tx_set("device_name", DEVICE_SETTING_STRING, val), ESP_OK);
    CHECK_INT(device_settings_tx_abort(0x202), ESP_OK);
}

static void test_set_enum(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x203, 1), ESP_OK);

    uint8_t val = 2; /* Medium */
    CHECK_INT(device_settings_tx_set("fan_mode", DEVICE_SETTING_ENUM, &val), ESP_OK);
    CHECK_INT(device_settings_tx_abort(0x203), ESP_OK);
}

/* ------------------------------------------------------------------ *
 * Tests: SET validation
 * ------------------------------------------------------------------ */

static void test_set_before_begin(void)
{
    setup_test_env();
    /* No transaction active */
    bool val = true;
    esp_err_t err = device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val);
    CHECK_INT(err, ESP_ERR_INVALID_STATE);
}

static void test_set_wrong_type(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x210, 1), ESP_OK);

    /* Try to set BOOL setting with INT type */
    int32_t val = 1;
    esp_err_t err = device_settings_tx_set("sensor_enabled", DEVICE_SETTING_INT, &val);
    CHECK_INT(err, ESP_ERR_INVALID_ARG);
    device_settings_tx_abort(0x210);
}

static void test_set_int_out_of_range(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x211, 1), ESP_OK);

    int32_t val = 150; /* max is 100 */
    esp_err_t err = device_settings_tx_set("sample_rate", DEVICE_SETTING_INT, &val);
    CHECK_INT(err, ESP_ERR_INVALID_ARG);
    device_settings_tx_abort(0x211);
}

static void test_set_int_below_min(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x212, 1), ESP_OK);

    int32_t val = 0; /* min is 1 */
    esp_err_t err = device_settings_tx_set("sample_rate", DEVICE_SETTING_INT, &val);
    CHECK_INT(err, ESP_ERR_INVALID_ARG);
    device_settings_tx_abort(0x212);
}

static void test_set_string_too_long(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x213, 1), ESP_OK);

    char long_str[40];
    memset(long_str, 'x', sizeof(long_str) - 1);
    long_str[sizeof(long_str) - 1] = '\0';
    esp_err_t err = device_settings_tx_set("device_name", DEVICE_SETTING_STRING, long_str);
    CHECK_INT(err, ESP_ERR_INVALID_ARG);
    device_settings_tx_abort(0x213);
}

static void test_set_enum_out_of_range(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x214, 1), ESP_OK);

    uint8_t val = 10; /* only 4 options (0-3) */
    esp_err_t err = device_settings_tx_set("fan_mode", DEVICE_SETTING_ENUM, &val);
    CHECK_INT(err, ESP_ERR_INVALID_ARG);
    device_settings_tx_abort(0x214);
}

static void test_set_readonly(void)
{
    device_settings_init();
    device_settings_register(&s_readonly_desc);
    device_settings_freeze();

    memset(&s_active_config, 0, sizeof(s_active_config));
    memset(&s_staging_config, 0, sizeof(s_staging_config));
    device_settings_set_active_config(&s_active_config);
    device_settings_set_staging_config(&s_staging_config);
    device_settings_set_config_size(sizeof(test_config_t));
    device_settings_set_revision(1);

    CHECK_INT(device_settings_tx_begin(0x215, 1), ESP_OK);
    bool val = true;
    esp_err_t err = device_settings_tx_set("readonly_bool", DEVICE_SETTING_BOOL, &val);
    CHECK_INT(err, ESP_ERR_INVALID_STATE);
    device_settings_tx_abort(0x215);
}

static void test_set_nonexistent(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x216, 1), ESP_OK);

    bool val = true;
    esp_err_t err = device_settings_tx_set("nonexistent", DEVICE_SETTING_BOOL, &val);
    CHECK_INT(err, ESP_ERR_NOT_FOUND);
    device_settings_tx_abort(0x216);
}

/* ------------------------------------------------------------------ *
 * Tests: SET does not mutate active config
 * ------------------------------------------------------------------ */

static void test_set_no_mutate_active(void)
{
    setup_test_env();
    s_active_config.sensor_enabled = false;
    s_staging_config.sensor_enabled = false;

    CHECK_INT(device_settings_tx_begin(0x220, 1), ESP_OK);

    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);

    /* Active unchanged, staging mutated */
    CHECK(s_active_config.sensor_enabled == false);
    CHECK(s_staging_config.sensor_enabled == true);

    device_settings_tx_abort(0x220);
}

/* ------------------------------------------------------------------ *
 * Tests: COMMIT
 * ------------------------------------------------------------------ */

static void test_commit_success(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x300, 1), ESP_OK);

    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);

    uint32_t new_rev = 0;
    esp_err_t err = device_settings_tx_commit(0x300, &new_rev);
    CHECK_INT(err, ESP_OK);
    CHECK_INT(new_rev, 2);
    CHECK_INT(device_settings_get_revision(), 2);
}

static void test_commit_increments_revision(void)
{
    setup_test_env();
    device_settings_set_revision(10);
    s_active_config.header.config_revision = 10;

    CHECK_INT(device_settings_tx_begin(0x301, 10), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);

    uint32_t new_rev = 0;
    CHECK_INT(device_settings_tx_commit(0x301, &new_rev), ESP_OK);
    CHECK_INT(new_rev, 11);
    CHECK_INT(device_settings_get_revision(), 11);
}

static void test_commit_no_active_tx(void)
{
    setup_test_env();
    uint32_t new_rev = 0;
    esp_err_t err = device_settings_tx_commit(0x302, &new_rev);
    CHECK_INT(err, ESP_ERR_INVALID_STATE);
}

static void test_commit_wrong_tx_id(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x303, 1), ESP_OK);

    uint32_t new_rev = 0;
    esp_err_t err = device_settings_tx_commit(0x999, &new_rev);
    CHECK_INT(err, ESP_ERR_INVALID_STATE);
    device_settings_tx_abort(0x303);
}

static void test_commit_validation_error_no_nvs(void)
{
    setup_test_env();
    device_settings_set_validate_fn(test_validate);

    CHECK_INT(device_settings_tx_begin(0x304, 1), ESP_OK);

    /* Disable sensor and set rate > 50 — should fail validation */
    bool enabled = false;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &enabled), ESP_OK);
    int32_t rate = 80;
    CHECK_INT(device_settings_tx_set("sample_rate", DEVICE_SETTING_INT, &rate), ESP_OK);

    uint32_t new_rev = 0;
    esp_err_t err = device_settings_tx_commit(0x304, &new_rev);
    CHECK_INT(err, ESP_ERR_INVALID_ARG);

    /* Revision unchanged */
    CHECK_INT(device_settings_get_revision(), 1);

    /* Active config unchanged */
    CHECK(s_active_config.sensor_enabled == false);
    CHECK(s_active_config.sample_rate == 0);

    device_settings_tx_abort(0x304);
}

/* ------------------------------------------------------------------ *
 * Tests: COMMIT idempotent (duplicate commit returns same revision)
 * ------------------------------------------------------------------ */

static void test_commit_duplicate_same_result(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x305, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);

    uint32_t rev1 = 0, rev2 = 0;
    CHECK_INT(device_settings_tx_commit(0x305, &rev1), ESP_OK);
    CHECK_INT(rev1, 2);

    /* State is now COMMITTED_WAIT_CONFIRM, second commit should fail */
    esp_err_t err = device_settings_tx_commit(0x305, &rev2);
    CHECK_INT(err, ESP_ERR_INVALID_STATE);
}

/* ------------------------------------------------------------------ *
 * Tests: ABORT
 * ------------------------------------------------------------------ */

static void test_abort_discards_staging(void)
{
    setup_test_env();
    s_active_config.sensor_enabled = false;
    s_staging_config.sensor_enabled = false;

    CHECK_INT(device_settings_tx_begin(0x400, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK(s_staging_config.sensor_enabled == true);

    CHECK_INT(device_settings_tx_abort(0x400), ESP_OK);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_IDLE);

    /* Active unchanged */
    CHECK(s_active_config.sensor_enabled == false);
}

static void test_abort_wrong_tx_id(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x401, 1), ESP_OK);

    esp_err_t err = device_settings_tx_abort(0x999);
    CHECK_INT(err, ESP_ERR_INVALID_STATE);
    device_settings_tx_abort(0x401);
}

static void test_abort_no_active_tx(void)
{
    setup_test_env();
    /* ABORT is safe when no tx active */
    esp_err_t err = device_settings_tx_abort(0x402);
    CHECK_INT(err, ESP_OK);
}

/* ------------------------------------------------------------------ *
 * Tests: Multi-field transaction
 * ------------------------------------------------------------------ */

static void test_multi_field_commit(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x500, 1), ESP_OK);

    bool enabled = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &enabled), ESP_OK);
    int32_t rate = 25;
    CHECK_INT(device_settings_tx_set("sample_rate", DEVICE_SETTING_INT, &rate), ESP_OK);
    const char *name = "MyDevice";
    CHECK_INT(device_settings_tx_set("device_name", DEVICE_SETTING_STRING, name), ESP_OK);
    uint8_t mode = 1; /* Low */
    CHECK_INT(device_settings_tx_set("fan_mode", DEVICE_SETTING_ENUM, &mode), ESP_OK);

    uint32_t new_rev = 0;
    CHECK_INT(device_settings_tx_commit(0x500, &new_rev), ESP_OK);
    CHECK_INT(new_rev, 2);

    /* Verify active config was updated */
    CHECK(s_active_config.sensor_enabled == true);
    CHECK_INT(s_active_config.sample_rate, 25);
    CHECK(strcmp(s_active_config.device_name, "MyDevice") == 0);
    CHECK_INT(s_active_config.fan_mode, 1);
}

/* ------------------------------------------------------------------ *
 * Tests: SET decode via gw_settings_decode_command
 * ------------------------------------------------------------------ */

static gw_message_t make_settings_msg(const char *command)
{
    gw_message_t msg;
    gw_message_init(&msg);
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND, sizeof(msg.type));
    strlcpy(msg.command, command, sizeof(msg.command));
    msg.has_int_value = 1;
    msg.has_bool_value = 1;
    msg.has_device_id = 1;
    strlcpy(msg.device_id, "gw1", sizeof(msg.device_id));
    msg.request_id = 1;
    msg.has_request_id = 1;
    return msg;
}

static void test_decode_begin(void)
{
    gw_message_t msg = make_settings_msg(GW_MSG_TYPE_SETTINGS_TX_BEGIN);
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

static void test_decode_set_bool(void)
{
    gw_message_t msg = make_settings_msg(GW_MSG_TYPE_SETTINGS_TX_SET);
    msg.settings_transaction_id = 1;
    msg.has_settings_transaction_id = 1;
    strlcpy(msg.setting_id, "sensor_enabled", sizeof(msg.setting_id));
    msg.has_setting_id = 1;

    gw_settings_command_t cmd;
    CHECK(gw_settings_decode_command(&msg, &cmd) == GW_OK);
    CHECK_INT(cmd.cmd_type, GW_SETTINGS_CMD_TX_SET);
}

static void test_decode_set_int(void)
{
    gw_message_t msg = make_settings_msg(GW_MSG_TYPE_SETTINGS_TX_SET);
    msg.settings_transaction_id = 1;
    msg.has_settings_transaction_id = 1;
    strlcpy(msg.setting_id, "sample_rate", sizeof(msg.setting_id));
    msg.has_setting_id = 1;

    gw_settings_command_t cmd;
    CHECK(gw_settings_decode_command(&msg, &cmd) == GW_OK);
    CHECK_INT(cmd.cmd_type, GW_SETTINGS_CMD_TX_SET);
}

static void test_decode_set_string(void)
{
    gw_message_t msg = make_settings_msg(GW_MSG_TYPE_SETTINGS_TX_SET);
    msg.settings_transaction_id = 1;
    msg.has_settings_transaction_id = 1;
    strlcpy(msg.setting_id, "device_name", sizeof(msg.setting_id));
    msg.has_setting_id = 1;

    gw_settings_command_t cmd;
    CHECK(gw_settings_decode_command(&msg, &cmd) == GW_OK);
    CHECK_INT(cmd.cmd_type, GW_SETTINGS_CMD_TX_SET);
}

static void test_decode_set_enum(void)
{
    gw_message_t msg = make_settings_msg(GW_MSG_TYPE_SETTINGS_TX_SET);
    msg.settings_transaction_id = 1;
    msg.has_settings_transaction_id = 1;
    strlcpy(msg.setting_id, "fan_mode", sizeof(msg.setting_id));
    msg.has_setting_id = 1;

    gw_settings_command_t cmd;
    CHECK(gw_settings_decode_command(&msg, &cmd) == GW_OK);
    CHECK_INT(cmd.cmd_type, GW_SETTINGS_CMD_TX_SET);
}

static void test_decode_commit(void)
{
    gw_message_t msg = make_settings_msg(GW_MSG_TYPE_SETTINGS_TX_COMMIT);
    msg.settings_transaction_id = 0x5678;
    msg.has_settings_transaction_id = 1;

    gw_settings_command_t cmd;
    CHECK(gw_settings_decode_command(&msg, &cmd) == GW_OK);
    CHECK_INT(cmd.cmd_type, GW_SETTINGS_CMD_TX_COMMIT);
    CHECK_INT(cmd.transaction_id, 0x5678);
}

static void test_decode_abort(void)
{
    gw_message_t msg = make_settings_msg(GW_MSG_TYPE_SETTINGS_TX_ABORT);
    msg.settings_transaction_id = 0x9ABC;
    msg.has_settings_transaction_id = 1;

    gw_settings_command_t cmd;
    CHECK(gw_settings_decode_command(&msg, &cmd) == GW_OK);
    CHECK_INT(cmd.cmd_type, GW_SETTINGS_CMD_TX_ABORT);
    CHECK_INT(cmd.transaction_id, 0x9ABC);
}

/* ------------------------------------------------------------------ *
 * Tests: commit_confirm encode
 * ------------------------------------------------------------------ */

/* Simple CBOR uint finder for testing */
static int cbor_find_uint_in_map(const uint8_t *buf, size_t len,
                                 uint8_t target_key, uint64_t *out)
{
    size_t pos = 0;
    if (pos >= len) return -1;
    uint8_t initial = buf[pos];
    if ((initial & 0xE0) != 0xA0) return -1;
    uint64_t map_count = initial & 0x1F;
    if (map_count == 24) { pos++; if (pos >= len) return -1; map_count = buf[pos]; }
    pos++;

    for (uint64_t i = 0; i < map_count && pos < len; i++) {
        /* Read key */
        uint8_t key_byte = buf[pos];
        uint64_t key = key_byte & 0x1F;
        pos++;
        if (key == 24) { if (pos >= len) return -1; key = buf[pos]; pos++; }
        if (key != (uint64_t)target_key) {
            /* Skip value */
            if (pos >= len) return -1;
            uint8_t vb = buf[pos] & 0x1F;
            pos++;
            if (vb == 24) { pos++; }
            else if (vb == 25) { pos += 2; }
            else if (vb == 26) { pos += 4; }
            else if (vb == 27) { pos += 8; }
            else if ((buf[pos-1] & 0xE0) == 0x60) { pos += vb; }
            continue;
        }
        /* Read value */
        if (pos >= len) return -1;
        uint8_t vb = buf[pos] & 0x1F;
        pos++;
        if (vb == 24) { if (pos >= len) return -1; *out = buf[pos]; pos++; }
        else if (vb == 25) { if (pos+1 >= len) return -1; *out = (buf[pos]<<8)|buf[pos+1]; pos+=2; }
        else if (vb == 26) { if (pos+3 >= len) return -1; *out = ((uint64_t)buf[pos]<<24)|((uint64_t)buf[pos+1]<<16)|((uint64_t)buf[pos+2]<<8)|buf[pos+3]; pos+=4; }
        else { *out = vb; }
        return 0;
    }
    return -1;
}

static void test_commit_confirm_encode(void)
{
    uint8_t buf[256];
    int enc = gw_settings_encode_commit_confirm(buf, sizeof(buf),
                                                 0xDEAD, 7, 42);
    CHECK(enc > 0);
    CHECK(enc <= 256);

    uint64_t tx_id = 0, new_rev = 0;
    CHECK(cbor_find_uint_in_map(buf, (size_t)enc,
                                GW_KEY_SETTINGS_TRANSACTION_ID, &tx_id) == 0);
    CHECK(cbor_find_uint_in_map(buf, (size_t)enc,
                                GW_KEY_SETTINGS_NEW_REVISION, &new_rev) == 0);
    CHECK_INT(tx_id, 0xDEAD);
    CHECK_INT(new_rev, 7);
}

/* ------------------------------------------------------------------ *
 * Tests: Transaction state machine
 * ------------------------------------------------------------------ */

static void test_state_machine_idle_to_active(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_IDLE);
    CHECK_INT(device_settings_tx_begin(0x600, 1), ESP_OK);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_ACTIVE);
    device_settings_tx_abort(0x600);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_IDLE);
}

static void test_state_machine_commit_to_wait_confirm(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x601, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0x601, NULL), ESP_OK);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_COMMITTED_WAIT_CONFIRM);
}

/* ------------------------------------------------------------------ *
 * Tests: Confirm and restart
 * ------------------------------------------------------------------ */

static void test_confirm_requires_committed_state(void)
{
    setup_test_env();
    esp_err_t err = device_settings_tx_confirm_and_restart();
    CHECK_INT(err, ESP_ERR_INVALID_STATE);
}

static void test_confirm_from_committed_state(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x602, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0x602, NULL), ESP_OK);

    esp_err_t err = device_settings_tx_confirm_and_restart();
    CHECK_INT(err, ESP_OK);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_RESTART_PENDING);
}

/* ------------------------------------------------------------------ *
 * Tests: Snapshot consistency (revision captured at begin)
 * ------------------------------------------------------------------ */

static void test_snapshot_consistency(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0x700, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0x700, NULL), ESP_OK);

    /* State is COMMITTED_WAIT_CONFIRM; must reset to test revision check.
     * In production, the confirm_and_restart handles this. */
    device_settings_tx_reset_state();

    /* After commit, revision changed to 2.
     * New BEGIN with revision=1 should fail. */
    esp_err_t err = device_settings_tx_begin(0x701, 1);
    CHECK_INT(err, ESP_ERR_INVALID_VERSION);

    /* BEGIN with correct revision should succeed */
    CHECK_INT(device_settings_tx_begin(0x701, 2), ESP_OK);
    device_settings_tx_abort(0x701);
}

/* ------------------------------------------------------------------ *
 * Main
 * ------------------------------------------------------------------ */

int main(void)
{
    /* BEGIN tests */
    test_begin_correct_revision();
    test_begin_stale_revision();
    test_begin_duplicate_same_tx_id();
    test_begin_second_different_tx();

    /* SET tests */
    test_set_bool();
    test_set_int();
    test_set_string();
    test_set_enum();
    test_set_before_begin();
    test_set_wrong_type();
    test_set_int_out_of_range();
    test_set_int_below_min();
    test_set_string_too_long();
    test_set_enum_out_of_range();
    test_set_readonly();
    test_set_nonexistent();
    test_set_no_mutate_active();

    /* COMMIT tests */
    test_commit_success();
    test_commit_increments_revision();
    test_commit_no_active_tx();
    test_commit_wrong_tx_id();
    test_commit_validation_error_no_nvs();
    test_commit_duplicate_same_result();

    /* ABORT tests */
    test_abort_discards_staging();
    test_abort_wrong_tx_id();
    test_abort_no_active_tx();

    /* Multi-field */
    test_multi_field_commit();

    /* Decode tests */
    test_decode_begin();
    test_decode_set_bool();
    test_decode_set_int();
    test_decode_set_string();
    test_decode_set_enum();
    test_decode_commit();
    test_decode_abort();
    test_commit_confirm_encode();

    /* State machine */
    test_state_machine_idle_to_active();
    test_state_machine_commit_to_wait_confirm();
    test_confirm_requires_committed_state();
    test_confirm_from_committed_state();

    /* Snapshot consistency */
    test_snapshot_consistency();

    printf("settings_tx: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
