/*
 * device_settings_persistence — NVS blob persistence.
 *
 * Manages atomic config blob storage with format version and config_revision.
 * All NVS operations are bounded and error-checked.
 */
#include "device_settings.h"

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "device_settings_persist";

/* ------------------------------------------------------------------ *
 * Internal forward declarations (from registry module)
 * ------------------------------------------------------------------ */

void device_settings_set_revision(uint32_t revision);
void device_settings_set_active_config(void *config);
void device_settings_set_staging_config(void *config);
size_t device_settings_get_config_size(void);
uint16_t device_settings_get_format_version(void);

/* ------------------------------------------------------------------ *
 * NVS namespace and key
 * ------------------------------------------------------------------ */

#define NVS_NAMESPACE  "device_cfg"
#define NVS_KEY_CONFIG "config"

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

esp_err_t device_settings_load(void)
{
    size_t config_size = device_settings_get_config_size();
    if (config_size == 0) {
        ESP_LOGE(TAG, "load: config size not set");
        return ESP_ERR_INVALID_STATE;
    }

    void *active = (void *)device_settings_get_active_config();
    if (active == NULL) {
        ESP_LOGE(TAG, "load: active config buffer not allocated");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "load: no saved config, using defaults");
        /* Initialize with default values */
        memset(active, 0, config_size);
        device_settings_blob_header_t *header = (device_settings_blob_header_t *)active;
        header->format_version = device_settings_get_format_version();
        header->config_revision = 0;
        device_settings_set_revision(0);
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Read config blob */
    size_t read_size = config_size;
    err = nvs_get_blob(nvs_handle, NVS_KEY_CONFIG, active, &read_size);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load: nvs_get_blob failed: %s", esp_err_to_name(err));
        return err;
    }

    if (read_size != config_size) {
        ESP_LOGW(TAG, "load: size mismatch (%zu != %zu)", read_size, config_size);
        /* Use defaults on size mismatch */
        memset(active, 0, config_size);
        device_settings_blob_header_t *header = (device_settings_blob_header_t *)active;
        header->format_version = device_settings_get_format_version();
        header->config_revision = 0;
        device_settings_set_revision(0);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Validate header */
    device_settings_blob_header_t *header = (device_settings_blob_header_t *)active;
    if (header->format_version != device_settings_get_format_version()) {
        ESP_LOGW(TAG, "load: format version mismatch (%u != %u)",
                 header->format_version, device_settings_get_format_version());
        /* Use defaults on version mismatch */
        memset(active, 0, config_size);
        header->format_version = device_settings_get_format_version();
        header->config_revision = 0;
        device_settings_set_revision(0);
        return ESP_ERR_INVALID_VERSION;
    }

    device_settings_set_revision(header->config_revision);
    ESP_LOGI(TAG, "load: revision=%lu", (unsigned long)header->config_revision);
    return ESP_OK;
}

esp_err_t device_settings_save(const void *config, size_t config_size,
                               uint32_t new_revision)
{
    if (config == NULL || config_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Write config blob atomically */
    err = nvs_set_blob(nvs_handle, NVS_KEY_CONFIG, config, config_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_set_blob failed: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    /* Commit to ensure atomicity */
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_commit failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "save: revision=%lu, %zu bytes",
             (unsigned long)new_revision, config_size);
    return ESP_OK;
}