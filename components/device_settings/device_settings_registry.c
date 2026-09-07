/*
 * device_settings_registry — Settings descriptor registry.
 *
 * Manages static const descriptors with pointer-only registry.
 * No heap allocation for descriptors/strings.
 */
#include "device_settings.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "device_settings";

/* ------------------------------------------------------------------ *
 * Internal state
 * ------------------------------------------------------------------ */

static struct {
    const device_setting_descriptor_t *descriptors[DEVICE_SETTING_MAX_COUNT];
    size_t count;
    bool frozen;
    uint32_t config_revision;
    size_t config_size;
    uint16_t format_version;
    device_settings_validate_fn validate_fn;

    /* Active config blob (in RAM, loaded from NVS) */
    void *active_config;

    /* Staging config blob (modified during transaction) */
    void *staging_config;
} s_registry;

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

esp_err_t device_settings_init(void)
{
    memset(&s_registry, 0, sizeof(s_registry));
    s_registry.config_revision = 0;
    device_settings_tx_reset_state();
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t device_settings_register(const device_setting_descriptor_t *desc)
{
    if (desc == NULL) return ESP_ERR_INVALID_ARG;
    if (s_registry.frozen) return ESP_ERR_INVALID_STATE;
    if (s_registry.count >= DEVICE_SETTING_MAX_COUNT) return ESP_ERR_NO_MEM;

    /* Validate required fields */
    if (desc->id == NULL || desc->id[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (desc->title == NULL || desc->title[0] == '\0') return ESP_ERR_INVALID_ARG;

    /* Check for duplicate ID */
    for (size_t i = 0; i < s_registry.count; i++) {
        if (strcmp(s_registry.descriptors[i]->id, desc->id) == 0) {
            ESP_LOGW(TAG, "duplicate setting ID: %s", desc->id);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* Validate type-specific fields */
    if (desc->type == DEVICE_SETTING_ENUM) {
        if (desc->options == NULL || desc->option_count == 0) {
            ESP_LOGW(TAG, "enum %s: missing options", desc->id);
            return ESP_ERR_INVALID_ARG;
        }
        if (desc->option_count > DEVICE_SETTING_ENUM_MAX_OPTIONS) {
            ESP_LOGW(TAG, "enum %s: too many options (%d > %d)",
                     desc->id, desc->option_count, DEVICE_SETTING_ENUM_MAX_OPTIONS);
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (desc->type == DEVICE_SETTING_INT) {
        if (desc->min_value > desc->max_value) {
            ESP_LOGW(TAG, "int %s: min > max", desc->id);
            return ESP_ERR_INVALID_ARG;
        }
        if (desc->step == 0) {
            ESP_LOGW(TAG, "int %s: step == 0", desc->id);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Store pointer (no heap copy per Phase 1 RAM policy) */
    s_registry.descriptors[s_registry.count++] = desc;
    ESP_LOGI(TAG, "registered: %s (type=%d, group=%s)",
             desc->id, desc->type, desc->group ? desc->group : "");

    return ESP_OK;
}

esp_err_t device_settings_freeze(void)
{
    if (s_registry.frozen) return ESP_OK;
    s_registry.frozen = true;
    ESP_LOGI(TAG, "frozen (%d settings)", (int)s_registry.count);
    return ESP_OK;
}

size_t device_settings_count(void)
{
    return s_registry.count;
}

const device_setting_descriptor_t *device_settings_get(size_t index)
{
    if (index >= s_registry.count) return NULL;
    return s_registry.descriptors[index];
}

const device_setting_descriptor_t *device_settings_find(const char *id)
{
    if (id == NULL) return NULL;
    for (size_t i = 0; i < s_registry.count; i++) {
        if (strcmp(s_registry.descriptors[i]->id, id) == 0) {
            return s_registry.descriptors[i];
        }
    }
    return NULL;
}

uint32_t device_settings_get_revision(void)
{
    return s_registry.config_revision;
}

/* ------------------------------------------------------------------ *
 * Internal accessors (used by transaction/persistence modules)
 * ------------------------------------------------------------------ */

void device_settings_set_revision(uint32_t revision)
{
    s_registry.config_revision = revision;
}

void device_settings_set_config_size(size_t size)
{
    s_registry.config_size = size;
}

void device_settings_set_format_version(uint16_t version)
{
    s_registry.format_version = version;
}

void device_settings_set_validate_fn(device_settings_validate_fn fn)
{
    s_registry.validate_fn = fn;
}

size_t device_settings_get_config_size(void)
{
    return s_registry.config_size;
}

uint16_t device_settings_get_format_version(void)
{
    return s_registry.format_version;
}

device_settings_validate_fn device_settings_get_validate_fn(void)
{
    return s_registry.validate_fn;
}

const void *device_settings_get_active_config(void)
{
    return s_registry.active_config;
}

void *device_settings_get_staging_config(void)
{
    return s_registry.staging_config;
}

void device_settings_set_active_config(void *config)
{
    s_registry.active_config = config;
}

void device_settings_set_staging_config(void *config)
{
    s_registry.staging_config = config;
}