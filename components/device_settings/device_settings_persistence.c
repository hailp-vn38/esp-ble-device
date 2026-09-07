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
device_settings_defaults_fn device_settings_get_defaults_fn(void);
device_settings_validate_fn device_settings_get_validate_fn(void);

/* ------------------------------------------------------------------ *
 * NVS namespace and key
 * ------------------------------------------------------------------ */

#define NVS_NAMESPACE  "device_cfg"
#define NVS_KEY_CONFIG "config"

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

static esp_err_t load_defaults(void *active, void *staging, size_t config_size)
{
    device_settings_defaults_fn defaults_fn = device_settings_get_defaults_fn();
    device_settings_validate_fn validate_fn = device_settings_get_validate_fn();
    if (defaults_fn == NULL) return ESP_ERR_INVALID_STATE;

    memset(active, 0, config_size);
    device_settings_blob_header_t *header = (device_settings_blob_header_t *)active;
    header->format_version = device_settings_get_format_version();
    header->reserved = 0;
    header->config_revision = 0;
    esp_err_t err = defaults_fn((uint8_t *)active + sizeof(*header),
                                config_size - sizeof(*header));
    if (err != ESP_OK) return err;
    if (validate_fn != NULL) {
        err = validate_fn(active, config_size);
        if (err != ESP_OK) return err;
    }
    memcpy(staging, active, config_size);
    device_settings_set_revision(0);
    return ESP_OK;
}

esp_err_t device_settings_load(device_settings_load_result_t *out_result)
{
    if (out_result != NULL) *out_result = DEVICE_SETTINGS_LOAD_FATAL;
    size_t config_size = device_settings_get_config_size();
    void *active = (void *)device_settings_get_active_config();
    void *staging = device_settings_get_staging_config();
    if (config_size < sizeof(device_settings_blob_header_t) ||
        active == NULL || staging == NULL) return ESP_ERR_INVALID_STATE;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = load_defaults(active, staging, config_size);
        if (err == ESP_OK && out_result != NULL)
            *out_result = DEVICE_SETTINGS_LOAD_DEFAULTS_NOT_FOUND;
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }
    if (err != ESP_OK) return err;

    size_t read_size = config_size;
    err = nvs_get_blob(nvs_handle, NVS_KEY_CONFIG, active, &read_size);
    nvs_close(nvs_handle);
    if (err != ESP_OK) return err;

    device_settings_blob_header_t *header = (device_settings_blob_header_t *)active;
    device_settings_validate_fn validate_fn = device_settings_get_validate_fn();
    bool valid = read_size == config_size &&
                 header->format_version == device_settings_get_format_version() &&
                 header->reserved == 0 &&
                 (validate_fn == NULL || validate_fn(active, config_size) == ESP_OK);
    if (!valid) {
        ESP_LOGW(TAG, "load: invalid persisted blob, recovering defaults");
        err = load_defaults(active, staging, config_size);
        if (err == ESP_OK && out_result != NULL)
            *out_result = DEVICE_SETTINGS_LOAD_DEFAULTS_RECOVERED;
        return err;
    }

    memcpy(staging, active, config_size);
    device_settings_set_revision(header->config_revision);
    if (out_result != NULL) *out_result = DEVICE_SETTINGS_LOAD_OK;
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