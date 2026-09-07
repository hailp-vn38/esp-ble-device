/*
 * device_settings_transaction — Settings transaction management.
 *
 * Implements BEGIN -> SET* -> COMMIT -> COMMIT_CONFIRM -> reboot flow.
 * All mutations happen on staging; active is only updated after NVS commit.
 *
 * Phase 4 additions:
 *   - Confirm timeout timer: restarts automatically if COMMIT_CONFIRM not received.
 *   - Disconnect-after-commit: schedules restart if BLE drops while WAIT_CONFIRM.
 *   - Idempotent duplicate handling: same tx_id COMMIT/CONFIRM returns deterministic result.
 *   - All state lives in RAM; no tx-id persistence needed post-reboot.
 */
#include "device_settings.h"

#include <string.h>
#include "esp_log.h"

#if !defined(GW_HOST_TEST) && defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#endif

static const char *TAG = "device_settings_tx";

/* ------------------------------------------------------------------ *
 * Internal forward declarations (from registry module)
 * ------------------------------------------------------------------ */

void device_settings_set_revision(uint32_t revision);
void device_settings_set_active_config(void *config);
void device_settings_set_staging_config(void *config);
const void *device_settings_get_active_config(void);
void *device_settings_get_staging_config(void);
size_t device_settings_get_config_size(void);
device_settings_validate_fn device_settings_get_validate_fn(void);
esp_err_t device_settings_save(const void *config, size_t config_size,
                               uint32_t new_revision);

/* ------------------------------------------------------------------ *
 * Restart scheduling hook (from device_app)
 * ------------------------------------------------------------------ */

extern esp_err_t device_app_schedule_restart(uint32_t delay_ms);

/* ------------------------------------------------------------------ *
 * Internal state
 *
 * RAM state kept minimal (per spec: last_committed_transaction_id,
 * last_committed_revision, state).
 * ------------------------------------------------------------------ */

static struct {
    device_settings_tx_state_t state;
    uint64_t transaction_id;
    uint32_t expected_revision;

    /* Phase 4: idempotent duplicate fields */
    uint64_t last_committed_tx_id;
    uint32_t last_committed_revision;

    /* Confirm timeout timer */
    void *timer_handle;
} s_tx;

/* ------------------------------------------------------------------ *
 * Confirm timeout (Phase 4)
 *
 * Uses the FreeRTOS timer from device_app. On timeout the device
 * transitions to RESTART_PENDING and schedules restart.
 * The timeout value is configurable via the constant below.
 * ------------------------------------------------------------------ */

#define CONFIRM_TIMEOUT_MS  5000  /* 5 seconds — bounded, configurable */

/* Note: confirm_timeout_cb is wired up at runtime by device_app via
 * FreeRTOS timer. Not called directly in host tests. */
__attribute__((unused))
void device_settings_confirm_timeout_cb(void *arg)
{
    (void)arg;
    if (s_tx.state == DEVICE_SETTINGS_TX_COMMITTED_WAIT_CONFIRM) {
        ESP_LOGW(TAG, "confirm timeout — scheduling restart");
        s_tx.state = DEVICE_SETTINGS_TX_RESTART_PENDING;
        device_app_schedule_restart(500);
    }
}

/* Start the confirm timeout timer (called after COMMIT succeeds). */
static void confirm_timer_start(void)
{
#if !defined(GW_HOST_TEST) && defined(ESP_PLATFORM)
    if (s_tx.timer_handle != NULL) {
        xTimerStop((TimerHandle_t)s_tx.timer_handle, 0);
        xTimerChangePeriod((TimerHandle_t)s_tx.timer_handle,
                           pdMS_TO_TICKS(CONFIRM_TIMEOUT_MS), 0);
        xTimerReset((TimerHandle_t)s_tx.timer_handle, 0);
        ESP_LOGI(TAG, "confirm timer started (%d ms)", CONFIRM_TIMEOUT_MS);
    }
#else
    (void)0; /* host tests: no timer */
#endif
}

/* Stop the confirm timeout timer (called on confirm, abort, disconnect). */
static void confirm_timer_stop(void)
{
#if !defined(GW_HOST_TEST) && defined(ESP_PLATFORM)
    if (s_tx.timer_handle != NULL) {
        xTimerStop((TimerHandle_t)s_tx.timer_handle, 0);
    }
#else
    (void)0;
#endif
}

/* ------------------------------------------------------------------ *
 * Internal helpers
 * ------------------------------------------------------------------ */

static void tx_abort_internal(void)
{
    confirm_timer_stop();
    s_tx.state = DEVICE_SETTINGS_TX_IDLE;
    s_tx.transaction_id = 0;
    s_tx.expected_revision = 0;
    ESP_LOGI(TAG, "transaction aborted");
}

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

void device_settings_tx_reset_state(void)
{
    memset(&s_tx, 0, sizeof(s_tx));
    s_tx.state = DEVICE_SETTINGS_TX_IDLE;
}

/* Set the confirm timeout timer handle (created by device_app). */
void device_settings_tx_set_confirm_timer(void *timer)
{
    s_tx.timer_handle = timer;
}

esp_err_t device_settings_tx_begin(uint64_t transaction_id,
                                   uint32_t expected_revision)
{
    /* Reject if another transaction is active */
    if (s_tx.state != DEVICE_SETTINGS_TX_IDLE) {
        ESP_LOGW(TAG, "begin: transaction already active (state=%d)", s_tx.state);
        return ESP_ERR_INVALID_STATE;
    }

    /* Check expected revision */
    uint32_t current_revision = device_settings_get_revision();
    if (expected_revision != current_revision) {
        ESP_LOGW(TAG, "begin: revision mismatch (expected=%lu, current=%lu)",
                 (unsigned long)expected_revision, (unsigned long)current_revision);
        return ESP_ERR_INVALID_VERSION;
    }

    /* Copy active -> staging */
    void *staging = device_settings_get_staging_config();
    const void *active = device_settings_get_active_config();
    size_t config_size = device_settings_get_config_size();

    if (staging == NULL || active == NULL || config_size == 0) {
        ESP_LOGE(TAG, "begin: config not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(staging, active, config_size);

    /* Start transaction */
    s_tx.state = DEVICE_SETTINGS_TX_ACTIVE;
    s_tx.transaction_id = transaction_id;
    s_tx.expected_revision = expected_revision;

    ESP_LOGI(TAG, "begin: tx_id=0x%llX, revision=%lu",
             (unsigned long long)transaction_id, (unsigned long)current_revision);

    return ESP_OK;
}

esp_err_t device_settings_tx_set(const char *setting_id,
                                 device_setting_type_t type,
                                 const void *value)
{
    if (s_tx.state != DEVICE_SETTINGS_TX_ACTIVE) {
        ESP_LOGW(TAG, "set: no active transaction");
        return ESP_ERR_INVALID_STATE;
    }

    if (setting_id == NULL || setting_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find the setting descriptor */
    const device_setting_descriptor_t *desc = device_settings_find(setting_id);
    if (desc == NULL) {
        ESP_LOGW(TAG, "set: unknown setting '%s'", setting_id);
        return ESP_ERR_NOT_FOUND;
    }

    /* Check readonly */
    if (desc->flags & DEVICE_SETTING_FLAG_READONLY) {
        ESP_LOGW(TAG, "set: setting '%s' is readonly", setting_id);
        return ESP_ERR_INVALID_STATE;
    }

    /* Check type match */
    if (desc->type != type) {
        ESP_LOGW(TAG, "set: type mismatch for '%s' (expected=%d, got=%d)",
                 setting_id, desc->type, type);
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate value based on type */
    if (type == DEVICE_SETTING_INT) {
        int32_t int_val = *(const int32_t *)value;
        if (int_val < desc->min_value || int_val > desc->max_value) {
            ESP_LOGW(TAG, "set: int value out of range for '%s' (%ld not in [%ld, %ld])",
                     setting_id, (long)int_val, (long)desc->min_value, (long)desc->max_value);
            return ESP_ERR_INVALID_ARG;
        }
        if (desc->step > 0 && ((int_val - desc->min_value) % desc->step) != 0) {
            ESP_LOGW(TAG, "set: int value not on step for '%s'", setting_id);
            return ESP_ERR_INVALID_ARG;
        }
    } else if (type == DEVICE_SETTING_STRING) {
        const char *str_val = (const char *)value;
        size_t len = strlen(str_val);
        if (desc->max_length > 0 && len > desc->max_length) {
            ESP_LOGW(TAG, "set: string too long for '%s' (%zu > %d)",
                     setting_id, len, desc->max_length);
            return ESP_ERR_INVALID_ARG;
        }
    } else if (type == DEVICE_SETTING_ENUM) {
        uint8_t enum_val = *(const uint8_t *)value;
        if (enum_val >= desc->option_count) {
            ESP_LOGW(TAG, "set: enum value out of range for '%s' (%d >= %d)",
                     setting_id, enum_val, desc->option_count);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Stage the value using typed callback when available. */
    esp_err_t stage_err = ESP_OK;
    void *stage_ctx = desc->stage_ctx != NULL ? desc->stage_ctx : desc->ctx;
    if (desc->stage_cb.stage_bool != NULL) {
        switch (type) {
        case DEVICE_SETTING_BOOL:
            stage_err = desc->stage_cb.stage_bool(stage_ctx, *(const bool *)value);
            break;
        case DEVICE_SETTING_INT:
            stage_err = desc->stage_cb.stage_int(stage_ctx, *(const int32_t *)value);
            break;
        case DEVICE_SETTING_STRING:
            stage_err = desc->stage_cb.stage_string(stage_ctx, (const char *)value,
                                                   strlen((const char *)value));
            break;
        case DEVICE_SETTING_ENUM: {
            device_setting_enum_value_t enum_value = { .index = *(const uint8_t *)value };
            stage_err = desc->stage_cb.stage_enum(stage_ctx, enum_value);
            break;
        }
        default:
            stage_err = ESP_ERR_INVALID_ARG;
            break;
        }
    } else if (desc->stage != NULL) {
        stage_err = desc->stage(stage_ctx, value);
    }
    if (stage_err != ESP_OK) {
        ESP_LOGW(TAG, "set: stage callback failed for '%s': %s",
                 setting_id, esp_err_to_name(stage_err));
        return stage_err;
    }

    ESP_LOGD(TAG, "set: '%s' staged", setting_id);
    return ESP_OK;
}

esp_err_t device_settings_tx_commit(uint64_t transaction_id,
                                    uint32_t *new_revision)
{
    if (s_tx.state != DEVICE_SETTINGS_TX_ACTIVE) {
        ESP_LOGW(TAG, "commit: no active transaction");
        return ESP_ERR_INVALID_STATE;
    }

    /* Verify transaction ID matches */
    if (s_tx.transaction_id != transaction_id) {
        ESP_LOGW(TAG, "commit: transaction ID mismatch");
        return ESP_ERR_INVALID_STATE;
    }

    /* Run cross-field validation if set */
    device_settings_validate_fn validate_fn = device_settings_get_validate_fn();
    if (validate_fn) {
        void *staging = device_settings_get_staging_config();
        size_t config_size = device_settings_get_config_size();
        esp_err_t err = validate_fn(staging, config_size);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "commit: validation failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* Increment revision */
    uint32_t current_revision = device_settings_get_revision();
    uint32_t updated_revision = current_revision + 1;

    /* Update revision in staging blob */
    void *staging = device_settings_get_staging_config();
    device_settings_blob_header_t *header = (device_settings_blob_header_t *)staging;
    header->config_revision = updated_revision;

    /* Persist atomically */
    size_t config_size = device_settings_get_config_size();
    esp_err_t err = device_settings_save(staging, config_size, updated_revision);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit: NVS save failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Promote staging -> active */
    void *active = (void *)device_settings_get_active_config();
    memcpy(active, staging, config_size);
    device_settings_set_revision(updated_revision);

    /* Phase 4: update state + idempotent fields */
    s_tx.state = DEVICE_SETTINGS_TX_COMMITTED_WAIT_CONFIRM;
    s_tx.last_committed_tx_id = transaction_id;
    s_tx.last_committed_revision = updated_revision;

    /* Start confirm timeout timer — if CONFIRM not received, auto-restart. */
    confirm_timer_start();

    if (new_revision) *new_revision = updated_revision;

    ESP_LOGI(TAG, "commit: tx_id=0x%llX, new_revision=%lu",
             (unsigned long long)transaction_id, (unsigned long)updated_revision);

    return ESP_OK;
}

esp_err_t device_settings_tx_abort(uint64_t transaction_id)
{
    if (s_tx.state == DEVICE_SETTINGS_TX_IDLE) {
        return ESP_OK; /* Safe to abort when idle */
    }

    if (s_tx.transaction_id != transaction_id) {
        ESP_LOGW(TAG, "abort: transaction ID mismatch");
        return ESP_ERR_INVALID_STATE;
    }

    tx_abort_internal();
    return ESP_OK;
}

device_settings_tx_state_t device_settings_tx_get_state(void)
{
    return s_tx.state;
}

esp_err_t device_settings_tx_confirm_and_restart(void)
{
    if (s_tx.state != DEVICE_SETTINGS_TX_COMMITTED_WAIT_CONFIRM) {
        ESP_LOGW(TAG, "confirm: not in committed_wait_confirm state");
        return ESP_ERR_INVALID_STATE;
    }

    /* Phase 4: record confirm, transition to RESTART_PENDING, schedule restart */
    ESP_LOGI(TAG, "confirm: tx_id=0x%llX, revision=%lu — scheduling restart",
             (unsigned long long)s_tx.last_committed_tx_id,
             (unsigned long)s_tx.last_committed_revision);

    confirm_timer_stop();
    s_tx.state = DEVICE_SETTINGS_TX_RESTART_PENDING;

    /* Schedule restart after short grace period (allows ACK to be delivered). */
    esp_err_t err = device_app_schedule_restart(500);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "confirm: schedule_restart failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 * Phase 4: Disconnect-after-commit handling
 *
 * If BLE disconnects while in COMMITTED_WAIT_CONFIRM, do NOT rollback
 * persisted config. Schedule restart immediately or after short grace.
 * The device will boot with the new revision; gateway reconciles
 * via read_settings.
 * ------------------------------------------------------------------ */

void device_settings_tx_on_disconnect(void)
{
    if (s_tx.state == DEVICE_SETTINGS_TX_COMMITTED_WAIT_CONFIRM) {
        ESP_LOGW(TAG, "disconnect in COMMITTED_WAIT_CONFIRM — scheduling restart");
        confirm_timer_stop();
        s_tx.state = DEVICE_SETTINGS_TX_RESTART_PENDING;
        device_app_schedule_restart(500);
    }
}

/* ------------------------------------------------------------------ *
 * Phase 4: Idempotent duplicate handling
 *
 * Returns the last committed revision if the tx_id matches.
 * Used for duplicate COMMIT/CONFIRM before reboot.
 * ------------------------------------------------------------------ */

bool device_settings_tx_get_last_committed(uint64_t *out_tx_id,
                                           uint32_t *out_revision)
{
    if (s_tx.last_committed_tx_id == 0) return false;
    if (out_tx_id) *out_tx_id = s_tx.last_committed_tx_id;
    if (out_revision) *out_revision = s_tx.last_committed_revision;
    return true;
}
