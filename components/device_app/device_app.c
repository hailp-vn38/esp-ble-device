/*
 * device_app — composition root (docs §9..#12, §86..#90).
 *
 * Orchestrates the 16-step boot sequence and lifecycle.
 *
 * Routing identity rules (spec D2, D3, D4):
 *   - s_app.device_id is NATIVE identity (model-derived, e.g. "esp32s3-ref")
 *   - Used ONLY for spontaneous device_event metadata (button_pressed, etc.)
 *   - NEVER used for ACK/capability response routing (those use request->device_id)
 *   - device_command_set_device_id() is DEPRECATED and NOT called here
 */
#include "device_app.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "gateway_protocol.h"
#include "ble_peripheral.h"
#include "device_command.h"
#include "device_event.h"
#include "device_feature.h"
#include "device_settings.h"

static const char *TAG = "device_app";

/* ------------------------------------------------------------------ *
 * Internal state
 * ------------------------------------------------------------------ */

static struct {
    const device_app_profile_t *profile;
    bool started;
    /* Native device identity (model-derived). Used ONLY for spontaneous
     * device_event metadata. NEVER used for ACK/capability routing
     * (spec D4). */
    char device_id[GW_MSG_DEVICE_ID_LEN];
} s_app;

/* ------------------------------------------------------------------ *
 * Restart timer (Phase 4 — safe restart helper)
 *
 * Never call esp_restart() from BLE GATT callback / notify task context.
 * A one-shot FreeRTOS timer fires after the requested delay, allowing
 * the current handler stack to unwind. The timer callback runs in the
 * FreeRTOS timer service task, which is a safe context for restart.
 * ------------------------------------------------------------------ */

static TimerHandle_t s_restart_timer = NULL;
static bool s_restart_pending = false;

static void restart_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    ESP_LOGW(TAG, "restart timer fired — rebooting now");
    s_restart_pending = false;
    /* Stop BLE advertising before restart. */
    ble_peripheral_stop();
    /* Brief delay to let NimBLE unwind. */
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    /* unreachable */
}

static void settings_confirm_timer_cb(TimerHandle_t xTimer)
{
    device_settings_confirm_timeout_cb((void *)xTimer);
}

/* ------------------------------------------------------------------ *
 * BLE notify bridge (ble_peripheral -> device_command/device_event)
 * ------------------------------------------------------------------ */

static int ble_notify_bridge(const uint8_t *data, size_t len)
{
    return ble_peripheral_notify(data, len);
}

/* ------------------------------------------------------------------ *
 * BLE state callback (Phase 4: disconnect-after-commit handling)
 * ------------------------------------------------------------------ */

static ble_peripheral_state_t s_prev_ble_state = BLE_PERIPH_STOPPED;

static void ble_state_callback(ble_peripheral_state_t state)
{
    /* On disconnect, check if settings expects a confirm.
     * Only act on ADVERTISING transition from a CONNECTED* state
     * (i.e. after BLE disconnect), not initial advertising on boot. */
    bool was_connected = (s_prev_ble_state == BLE_PERIPH_CONNECTED ||
                          s_prev_ble_state == BLE_PERIPH_SECURING ||
                          s_prev_ble_state == BLE_PERIPH_WAIT_CCCD ||
                          s_prev_ble_state == BLE_PERIPH_READY);

    if (state == BLE_PERIPH_ADVERTISING && was_connected) {
        device_settings_tx_on_disconnect();
    }

    s_prev_ble_state = state;
}

/* ------------------------------------------------------------------ *
 * Lifecycle callbacks wired into device_command
 * ------------------------------------------------------------------ */

/* RX bridge: ble_peripheral RX queue -> device_command submit.
 * Called by the command worker task, not directly from NimBLE. */
static void ble_rx_bridge(const uint8_t *data, size_t len)
{
    device_command_submit(data, len);
}

/* ------------------------------------------------------------------ *
 * Boot sequence (doc §86)
 * ------------------------------------------------------------------ */

device_app_result_t device_app_start(void)
{
    if (s_app.profile == NULL) {
        ESP_LOGE(TAG, "no profile set");
        return DEVICE_APP_ERR_INVALID_STATE;
    }
    if (s_app.started) return DEVICE_APP_ERR_INVALID_STATE;

    const device_app_profile_t *p = s_app.profile;
    int rc = 0;

    /* Step 1: Logging already running. */

    /* Step 2: NVS init. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: erasing and re-init");
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Settings core must be initialized and bound before product registration.
     * Product callbacks only register static descriptors and defaults. */
    if (p->register_settings || p->settings_config_size > 0) {
        if (p->settings_active_config == NULL ||
            p->settings_staging_config == NULL ||
            p->settings_config_size == 0) {
            ESP_LOGE(TAG, "Settings profile has no valid storage binding");
            return DEVICE_APP_ERR_INVALID_ARG;
        }
        ret = device_settings_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "device_settings_init failed: %d", ret);
            return DEVICE_APP_ERR_STORAGE;
        }
        device_settings_storage_config_t settings_config = {
            .config_size = p->settings_config_size,
            .format_version = p->settings_format_version != 0 ?
                              p->settings_format_version : 1,
            .active_config = p->settings_active_config,
            .staging_config = p->settings_staging_config,
            .defaults_fn = p->settings_defaults_fn,
            .validate_fn = p->settings_validate_fn,
        };
        ret = device_settings_configure(&settings_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "device_settings_configure failed: %d", ret);
            return DEVICE_APP_ERR_STORAGE;
        }
        if (p->register_settings) {
            rc = p->register_settings();
            if (rc != 0) {
                ESP_LOGE(TAG, "register_settings failed: %d", rc);
                return DEVICE_APP_ERR_PRODUCT;
            }
        }
        ret = device_settings_freeze();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "device_settings_freeze failed: %d", ret);
            return DEVICE_APP_ERR_STORAGE;
        }
        device_settings_load_result_t load_result;
        (void)device_settings_load(&load_result);
    }

    /* Product init consumes committed configuration after Settings load. */
    if (p->product_init) {
        rc = p->product_init();
        if (rc != 0) {
            ESP_LOGE(TAG, "product_init failed: %d", rc);
            return DEVICE_APP_ERR_PRODUCT;
        }
    }

    /* Step 7: Protocol check (already validated at compile time). */
    ESP_LOGI(TAG, "protocol v%u, service 0x%04X",
             (unsigned)p->protocol_version,
             (unsigned)GW_BLE_SERVICE_UUID);

    /* Step 8: semantic feature registry init. */
    rc = device_feature_init();
    if (rc != 0) return DEVICE_APP_ERR_PRODUCT;

    /* Step 9: device_command init. */
    rc = device_command_init(ble_notify_bridge);
    if (rc != 0) return DEVICE_APP_ERR_COMMAND;
    device_command_set_capability_revision(
        p->capability_revision != 0 ? p->capability_revision : 1);

    /* Step 10-11: register common + product commands. */
    if (p->register_commands) {
        rc = p->register_commands();
        if (rc != 0) return DEVICE_APP_ERR_COMMAND;
    }

    /* Register semantic features before command/discovery freeze. */
    if (p->register_features) {
        rc = p->register_features();
        if (rc != 0) return DEVICE_APP_ERR_PRODUCT;
    }

    /* Freeze command registry and semantic snapshot. */
    rc = device_command_freeze();
    if (rc != 0) {
        ESP_LOGE(TAG, "command/feature binding validation failed");
        return DEVICE_APP_ERR_COMMAND;
    }

    /* Step 12: device_event init. */
    rc = device_event_init(ble_notify_bridge, s_app.device_id);
    if (rc != 0) return DEVICE_APP_ERR_EVENT;

    /* Step 13: product event registration. */
    if (p->register_events) {
        rc = p->register_events();
        if (rc != 0) return DEVICE_APP_ERR_EVENT;
    }

    /* Step 14: ble_peripheral init. */
    ble_peripheral_config_t ble_cfg = {
        .device_name = p->ble_name_prefix,
        .require_bonding = true,
        .adv_interval_ms = 100,
    };
    rc = ble_peripheral_init(&ble_cfg, ble_rx_bridge, ble_state_callback);
    if (rc != 0) return DEVICE_APP_ERR_BLE;

    /* Step 15: Start product. */
    if (p->product_start) {
        rc = p->product_start();
        if (rc != 0) return DEVICE_APP_ERR_PRODUCT;
    }

    /* Step 16: Start BLE (advertising). */
    rc = ble_peripheral_start();
    if (rc != 0) return DEVICE_APP_ERR_BLE;

    /* Step 17: Create one-shot restart timer (Phase 4). */
    if (s_restart_timer == NULL) {
        s_restart_timer = xTimerCreate(
            "settings_restart",
            pdMS_TO_TICKS(1000),   /* period — overwritten per-shot */
            pdFALSE,               /* one-shot */
            NULL,                  /* timer ID not used */
            restart_timer_cb);
        if (s_restart_timer == NULL) {
            ESP_LOGE(TAG, "restart timer create failed");
            return DEVICE_APP_ERR_NO_RESOURCE;
        }
    }

    /* Step 18: Create confirm timeout timer (Phase 4).
     * If COMMIT_CONFIRM not received within CONFIRM_TIMEOUT_MS,
     * device auto-restarts with persisted config. */
    {
        static TimerHandle_t s_confirm_timer = NULL;
        if (s_confirm_timer == NULL) {
            s_confirm_timer = xTimerCreate(
                "settings_confirm",
                pdMS_TO_TICKS(5000),  /* period — overwritten per-shot */
                pdFALSE,              /* one-shot */
                NULL,
                settings_confirm_timer_cb);
            if (s_confirm_timer == NULL) {
                ESP_LOGE(TAG, "confirm timer create failed");
                return DEVICE_APP_ERR_NO_RESOURCE;
            }
            device_settings_tx_set_confirm_timer(s_confirm_timer);
        }
    }

    s_app.started = true;
    ESP_LOGI(TAG, "started (model=%s)",
             p->model ? p->model : "?");
    return DEVICE_APP_OK;
}

/* ------------------------------------------------------------------ *
 * Stop sequence (doc §88)
 * ------------------------------------------------------------------ */

device_app_result_t device_app_stop(void)
{
    if (!s_app.started) return DEVICE_APP_ERR_INVALID_STATE;

    /* Stop advertising, disconnect, stop product, flush events. */
    ble_peripheral_stop();
    device_event_flush();

    if (s_app.profile && s_app.profile->product_stop) {
        s_app.profile->product_stop();
    }

    s_app.started = false;
    ESP_LOGI(TAG, "stopped");
    return DEVICE_APP_OK;
}

/* ------------------------------------------------------------------ *
 * Status / factory reset
 * ------------------------------------------------------------------ */

device_app_result_t device_app_get_status(device_app_status_t *out_status)
{
    if (out_status == NULL) return DEVICE_APP_ERR_INVALID_ARG;
    memset(out_status, 0, sizeof(*out_status));
    out_status->ble_state = ble_peripheral_get_state();
    out_status->ready = ble_peripheral_is_ready();
    out_status->mtu = ble_peripheral_get_mtu();
    return DEVICE_APP_OK;
}

device_app_result_t device_app_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset: clearing bonds");
    ble_peripheral_clear_bonds();
    return DEVICE_APP_OK;
}

/* ------------------------------------------------------------------ *
 * Profile setter
 * ------------------------------------------------------------------ */

device_app_result_t device_app_set_profile(const device_app_profile_t *profile)
{
    if (profile == NULL) return DEVICE_APP_ERR_INVALID_ARG;
    s_app.profile = profile;

    /* Build native device_id from model (spec D4).
     * This is NATIVE identity for events only, NOT Gateway routing ID.
     * ACK routing always uses request->device_id from incoming commands. */
    if (profile->model) {
        strlcpy(s_app.device_id, profile->model, sizeof(s_app.device_id));
    }
    return DEVICE_APP_OK;
}

/* ------------------------------------------------------------------ *
 * Restart scheduling API (Phase 4)
 * ------------------------------------------------------------------ */

device_app_result_t device_app_schedule_restart(uint32_t delay_ms)
{
    if (s_restart_timer == NULL) {
        ESP_LOGE(TAG, "schedule_restart: timer not created");
        return DEVICE_APP_ERR_INVALID_STATE;
    }

    /* Use portMAX_DELAY to guarantee timer command is accepted.
     * Stop previous timer, change period, then start fresh. */
    xTimerStop(s_restart_timer, portMAX_DELAY);
    xTimerChangePeriod(s_restart_timer, pdMS_TO_TICKS(delay_ms), portMAX_DELAY);
    xTimerStart(s_restart_timer, portMAX_DELAY);
    s_restart_pending = true;

    ESP_LOGW(TAG, "restart scheduled in %lu ms", (unsigned long)delay_ms);
    return DEVICE_APP_OK;
}

device_app_result_t device_app_cancel_restart(void)
{
    if (s_restart_timer == NULL) return DEVICE_APP_ERR_INVALID_STATE;
    if (!s_restart_pending) return DEVICE_APP_OK;

    xTimerStop(s_restart_timer, portMAX_DELAY);
    s_restart_pending = false;
    ESP_LOGI(TAG, "restart cancelled");
    return DEVICE_APP_OK;
}

bool device_app_is_restart_scheduled(void)
{
    return s_restart_pending;
}
