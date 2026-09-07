/*
 * Host unit tests for Settings v2 Phase 4 — Confirm, Restart & Reconcile.
 *
 * Tests the commit-confirm-restart flow, disconnect-after-commit handling,
 * idempotent duplicate handling, and error cases.
 *
 * Mocked: device_app_schedule_restart (captures delay_ms).
 *         device_settings_save (stores to mock NVS).
 *
 * Run: test/host/run_settings_confirm_tests.sh
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
extern void device_settings_confirm_timeout_cb(void *arg);

/* ------------------------------------------------------------------ *
 * Mock device_app_schedule_restart
 *
 * Captures the delay instead of actually rebooting.
 * ------------------------------------------------------------------ */

static int g_restart_schedule_count = 0;
static uint32_t g_restart_delay_ms = 0;
static bool g_restart_scheduled = false;

esp_err_t device_app_schedule_restart(uint32_t delay_ms)
{
    g_restart_schedule_count++;
    g_restart_delay_ms = delay_ms;
    g_restart_scheduled = true;
    return ESP_OK;
}

static void reset_restart_mock(void)
{
    g_restart_schedule_count = 0;
    g_restart_delay_ms = 0;
    g_restart_scheduled = false;
}

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

    reset_restart_mock();
}

/* ------------------------------------------------------------------ *
 * Tests: COMMIT enters wait-confirm, not restart immediately
 * ------------------------------------------------------------------ */

static void test_commit_enters_wait_confirm_not_restart(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0xA00, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0xA00, NULL), ESP_OK);

    /* State must be COMMITTED_WAIT_CONFIRM, not RESTART_PENDING */
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_COMMITTED_WAIT_CONFIRM);

    /* Restart must NOT be scheduled yet */
    CHECK(!g_restart_scheduled);

    device_settings_tx_reset_state();
}

/* ------------------------------------------------------------------ *
 * Tests: correct confirm schedules restart
 * ------------------------------------------------------------------ */

static void test_confirm_schedules_restart(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0xA01, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0xA01, NULL), ESP_OK);

    reset_restart_mock();

    /* Confirm */
    esp_err_t err = device_settings_tx_confirm_and_restart();
    CHECK_INT(err, ESP_OK);

    /* Restart scheduled */
    CHECK(g_restart_scheduled);
    CHECK_INT(g_restart_schedule_count, 1);
    CHECK(g_restart_delay_ms > 0);

    /* State is RESTART_PENDING */
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_RESTART_PENDING);
}

/* ------------------------------------------------------------------ *
 * Tests: confirm from wrong state rejected
 * ------------------------------------------------------------------ */

static void test_confirm_wrong_state_idle(void)
{
    setup_test_env();
    /* No transaction at all */
    esp_err_t err = device_settings_tx_confirm_and_restart();
    CHECK_INT(err, ESP_ERR_INVALID_STATE);
    CHECK(!g_restart_scheduled);
}

static void test_confirm_wrong_state_active(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0xA02, 1), ESP_OK);
    /* State is ACTIVE, not COMMITTED_WAIT_CONFIRM */
    esp_err_t err = device_settings_tx_confirm_and_restart();
    CHECK_INT(err, ESP_ERR_INVALID_STATE);
    device_settings_tx_abort(0xA02);
}

/* ------------------------------------------------------------------ *
 * Tests: duplicate confirm is safe (idempotent)
 * ------------------------------------------------------------------ */

static void test_duplicate_confirm_safe(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0xA03, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0xA03, NULL), ESP_OK);

    /* First confirm: OK */
    CHECK_INT(device_settings_tx_confirm_and_restart(), ESP_OK);
    CHECK(g_restart_scheduled);

    /* Second confirm: rejected (state is RESTART_PENDING, not WAIT_CONFIRM) */
    esp_err_t err = device_settings_tx_confirm_and_restart();
    CHECK_INT(err, ESP_ERR_INVALID_STATE);

    /* Restart still scheduled (not double-scheduled) */
    CHECK_INT(g_restart_schedule_count, 1);
}

/* ------------------------------------------------------------------ *
 * Tests: disconnect during COMMITTED_WAIT_CONFIRM schedules restart
 * ------------------------------------------------------------------ */

static void test_disconnect_schedules_restart(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0xA04, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0xA04, NULL), ESP_OK);
    reset_restart_mock();

    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_COMMITTED_WAIT_CONFIRM);

    /* Simulate BLE disconnect */
    device_settings_tx_on_disconnect();

    /* Restart scheduled */
    CHECK(g_restart_scheduled);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_RESTART_PENDING);
}

static void test_disconnect_idle_noop(void)
{
    setup_test_env();
    /* Idle state — disconnect should be a noop */
    device_settings_tx_on_disconnect();
    CHECK(!g_restart_scheduled);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_IDLE);
}

static void test_disconnect_active_noop(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0xA05, 1), ESP_OK);
    /* Still in ACTIVE state — disconnect should not schedule restart */
    device_settings_tx_on_disconnect();
    CHECK(!g_restart_scheduled);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_ACTIVE);
    device_settings_tx_abort(0xA05);
}

/* ------------------------------------------------------------------ *
 * Tests: idempotent last_committed fields
 * ------------------------------------------------------------------ */

static void test_last_committed_after_commit(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0xA06, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0xA06, NULL), ESP_OK);

    uint64_t tx_id = 0;
    uint32_t revision = 0;
    bool found = device_settings_tx_get_last_committed(&tx_id, &revision);
    CHECK(found);
    CHECK_INT(tx_id, 0xA06);
    CHECK_INT(revision, 2);

    device_settings_tx_reset_state();
}

static void test_last_committed_none_before_commit(void)
{
    setup_test_env();
    uint64_t tx_id = 0;
    uint32_t revision = 0;
    bool found = device_settings_tx_get_last_committed(&tx_id, &revision);
    CHECK(!found);
}

/* ------------------------------------------------------------------ *
 * Tests: commit preserves persisted config (no rollback on abort)
 * ------------------------------------------------------------------ */

static void test_commit_persists_config(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0xA07, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0xA07, NULL), ESP_OK);

    /* Active config now reflects committed changes */
    const test_config_t *active = (const test_config_t *)device_settings_get_active_config();
    CHECK(active->sensor_enabled == true);
    CHECK_INT(active->header.config_revision, 2);
}

/* ------------------------------------------------------------------ *
 * Tests: commit then abort does NOT rollback persisted config
 * ------------------------------------------------------------------ */

static void test_abort_after_commit_no_rollback(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0xA08, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0xA08, NULL), ESP_OK);

    /* ABORT with matching tx_id on COMMITTED_WAIT_CONFIRM.
     * The persisted config must remain unchanged. */
    device_settings_tx_abort(0xA08);

    const test_config_t *active = (const test_config_t *)device_settings_get_active_config();
    CHECK(active->sensor_enabled == true);
    CHECK_INT(active->header.config_revision, 2);

    device_settings_tx_reset_state();
}

/* ------------------------------------------------------------------ *
 * Tests: multiple fields committed atomically
 * ------------------------------------------------------------------ */

static void test_multi_field_commit_confirm(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0xA09, 1), ESP_OK);

    bool bval = true;
    int32_t ival = 50;
    const char *sval = "test-device";
    uint8_t eval = 2;

    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &bval), ESP_OK);
    CHECK_INT(device_settings_tx_set("sample_rate", DEVICE_SETTING_INT, &ival), ESP_OK);
    CHECK_INT(device_settings_tx_set("device_name", DEVICE_SETTING_STRING, sval), ESP_OK);
    CHECK_INT(device_settings_tx_set("fan_mode", DEVICE_SETTING_ENUM, &eval), ESP_OK);

    CHECK_INT(device_settings_tx_commit(0xA09, NULL), ESP_OK);
    CHECK_INT(device_settings_tx_confirm_and_restart(), ESP_OK);

    const test_config_t *active = (const test_config_t *)device_settings_get_active_config();
    CHECK(active->sensor_enabled == true);
    CHECK_INT(active->sample_rate, 50);
    CHECK(strcmp(active->device_name, "test-device") == 0);
    CHECK_INT(active->fan_mode, 2);
}

/* ------------------------------------------------------------------ *
 * Tests: begin after commit with new revision succeeds
 * ------------------------------------------------------------------ */

static void test_begin_after_commit_new_revision(void)
{
    setup_test_env();
    CHECK_INT(device_settings_tx_begin(0xA0A, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0xA0A, NULL), ESP_OK);

    /* Reset state to simulate confirm already handled */
    device_settings_tx_reset_state();

    /* New BEGIN with revision 2 should succeed */
    CHECK_INT(device_settings_tx_begin(0xA0B, 2), ESP_OK);
    device_settings_tx_abort(0xA0B);
}

/* ------------------------------------------------------------------ *
 * Tests: decode commit_confirm
 * ------------------------------------------------------------------ */

static void test_decode_commit_confirm(void)
{
    gw_message_t msg = { 0 };
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(msg.command, GW_MSG_TYPE_SETTINGS_COMMIT_CONFIRM, sizeof(msg.command));
    msg.has_request_id = true;
    msg.request_id = 42;
    msg.has_settings_transaction_id = true;
    msg.settings_transaction_id = 0xDEAD;
    msg.has_settings_new_revision = true;
    msg.settings_new_revision = 7;

    gw_settings_command_t cmd = { 0 };
    int rc = gw_settings_decode_command(&msg, &cmd);
    CHECK_INT(rc, GW_OK);
    CHECK_INT(cmd.cmd_type, GW_SETTINGS_CMD_TX_CONFIRM);
    CHECK(cmd.has_transaction_id);
    CHECK_INT(cmd.transaction_id, 0xDEAD);
    CHECK(cmd.has_new_revision);
    CHECK_INT(cmd.new_revision, 7);
}

/* ------------------------------------------------------------------ *
 * Confirm timeout fallback
 * ------------------------------------------------------------------ */

static void test_confirm_timeout_schedules_restart(void)
{
    setup_test_env();
    reset_restart_mock();

    /* COMMIT -> COMMITTED_WAIT_CONFIRM */
    CHECK_INT(device_settings_tx_begin(0xF00, 1), ESP_OK);
    bool val = true;
    CHECK_INT(device_settings_tx_set("sensor_enabled", DEVICE_SETTING_BOOL, &val), ESP_OK);
    CHECK_INT(device_settings_tx_commit(0xF00, NULL), ESP_OK);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_COMMITTED_WAIT_CONFIRM);
    CHECK(!g_restart_scheduled);

    /* Simulate timeout — callback transitions to RESTART_PENDING */
    device_settings_confirm_timeout_cb(NULL);

    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_RESTART_PENDING);
    CHECK(g_restart_scheduled);
    CHECK_INT(g_restart_schedule_count, 1);
}

static void test_confirm_timeout_noop_if_not_wait_confirm(void)
{
    setup_test_env();
    reset_restart_mock();

    /* Timeout in IDLE state — should do nothing */
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_IDLE);
    device_settings_confirm_timeout_cb(NULL);
    CHECK_INT(device_settings_tx_get_state(), DEVICE_SETTINGS_TX_IDLE);
    CHECK(!g_restart_scheduled);
}

/* ------------------------------------------------------------------ *
 * Main
 * ------------------------------------------------------------------ */

int main(void)
{
    /* Phase 4: Commit-Confirm-Reboot flow */
    test_commit_enters_wait_confirm_not_restart();
    test_confirm_schedules_restart();
    test_confirm_wrong_state_idle();
    test_confirm_wrong_state_active();
    test_duplicate_confirm_safe();

    /* Disconnect-after-commit */
    test_disconnect_schedules_restart();
    test_disconnect_idle_noop();
    test_disconnect_active_noop();

    /* Idempotent last-committed fields */
    test_last_committed_after_commit();
    test_last_committed_none_before_commit();

    /* Config persistence */
    test_commit_persists_config();
    test_abort_after_commit_no_rollback();
    test_multi_field_commit_confirm();
    test_begin_after_commit_new_revision();

    /* Decode */
    test_decode_commit_confirm();

    /* Confirm timeout fallback */
    test_confirm_timeout_schedules_restart();
    test_confirm_timeout_noop_if_not_wait_confirm();

    printf("settings_confirm: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
