/*
 * device_settings_restart — Reboot logic for settings.
 *
 * Manages safe reboot after commit confirm.
 * Phase 4 will implement actual reboot via esp_restart().
 */
#include "device_settings.h"

#include "esp_log.h"

static const char *TAG = "device_settings_restart";

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

esp_err_t device_settings_restart(void)
{
    ESP_LOGW(TAG, "restart: reboot not yet implemented (Phase 4)");
    /* TODO: Phase 4 will implement:
     * 1. Flush any pending events
     * 2. Stop BLE advertising
     * 3. Call esp_restart()
     */
    return ESP_OK;
}

bool device_settings_is_restart_pending(void)
{
    return device_settings_tx_get_state() == DEVICE_SETTINGS_TX_RESTART_PENDING;
}