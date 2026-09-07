/*
 * reference_config — Settings registration & callbacks (Phase 5).
 *
 * Registers 7 demo settings covering all types:
 *   sensor_enabled     BOOL    writable
 *   sample_interval    INT     1..3600 s, step 1
 *   target_temperature INT     30..120 °C
 *   fan_mode           ENUM    Off/Manual/Auto
 *   device_label       STRING  max 32
 *   admin_token        STRING  SECRET
 *   serial_number      STRING  READONLY
 *
 * Cross-field validation:
 *   If sensor_enabled is false, sample_interval must be 0 (unused).
 */
#include "reference_config.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "ref_config";

/* ------------------------------------------------------------------ *
 * Active / staging config instances
 * ------------------------------------------------------------------ */

static reference_config_t s_active_config;
static reference_config_t s_staging_config;

/* ------------------------------------------------------------------ *
 * Callback: read helpers
 * ------------------------------------------------------------------ */

static esp_err_t read_bool(void *ctx, void *out)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    *(bool *)out = cfg->sensor_enabled;
    return ESP_OK;
}

static esp_err_t read_int_interval(void *ctx, void *out)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    *(int32_t *)out = cfg->sample_interval;
    return ESP_OK;
}

static esp_err_t read_int_temp(void *ctx, void *out)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    *(int32_t *)out = cfg->target_temperature;
    return ESP_OK;
}

static esp_err_t read_enum_fan(void *ctx, void *out)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    *(int32_t *)out = cfg->fan_mode;
    return ESP_OK;
}

static esp_err_t read_string_label(void *ctx, void *out)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    strlcpy((char *)out, cfg->device_label, sizeof(cfg->device_label));
    return ESP_OK;
}

static esp_err_t read_secret_token(void *ctx, void *out)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    device_setting_secret_value_t *secret = (device_setting_secret_value_t *)out;
    secret->configured = cfg->admin_token_set;
    /* Never return plaintext — only configured status */
    return ESP_OK;
}

static esp_err_t read_string_serial(void *ctx, void *out)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    strlcpy((char *)out, cfg->serial_number, sizeof(cfg->serial_number));
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 * Callback: stage helpers
 * ------------------------------------------------------------------ */

static esp_err_t stage_bool(void *ctx, const void *value)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    cfg->sensor_enabled = *(const bool *)value;
    return ESP_OK;
}

static esp_err_t stage_int_interval(void *ctx, const void *value)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    cfg->sample_interval = *(const int32_t *)value;
    return ESP_OK;
}

static esp_err_t stage_int_temp(void *ctx, const void *value)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    cfg->target_temperature = *(const int32_t *)value;
    return ESP_OK;
}

static esp_err_t stage_enum_fan(void *ctx, const void *value)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    cfg->fan_mode = (int32_t)(*(const uint8_t *)value);
    return ESP_OK;
}

static esp_err_t stage_string_label(void *ctx, const void *value)
{
    reference_config_t *cfg = (reference_config_t *)ctx;
    strlcpy(cfg->device_label, (const char *)value,
            sizeof(cfg->device_label));
    return ESP_OK;
}

static esp_err_t stage_secret_token(void *ctx, const void *value)
{
    (void)value;
    reference_config_t *cfg = (reference_config_t *)ctx;
    /* Simulate: mark as configured, store a fake hash. */
    cfg->admin_token_set = true;
    strlcpy(cfg->admin_token_hash, "hashed", sizeof(cfg->admin_token_hash));
    ESP_LOGI(TAG, "admin_token set (hashed)");
    return ESP_OK;
}

/* serial_number: no stage callback — readonly, cannot be written. */

/* ------------------------------------------------------------------ *
 * Enum options
 * ------------------------------------------------------------------ */

static const device_setting_option_t s_fan_options[] = {
    { .label = "Off" },
    { .label = "Manual" },
    { .label = "Auto" },
};

/* ------------------------------------------------------------------ *
 * Setting descriptors (static const in flash)
 * ------------------------------------------------------------------ */

static const device_setting_descriptor_t s_settings[] = {
    {
        .id = "sensor_enabled",
        .title = "Sensor Enabled",
        .group = "sensor",
        .unit = "",
        .type = DEVICE_SETTING_BOOL,
        .flags = 0,
        .read = read_bool,
        .stage = stage_bool,
        .ctx = &s_staging_config,
    },
    {
        .id = "sample_interval",
        .title = "Sample Interval",
        .group = "sensor",
        .unit = "s",
        .type = DEVICE_SETTING_INT,
        .flags = 0,
        .min_value = 1,
        .max_value = 3600,
        .step = 1,
        .read = read_int_interval,
        .stage = stage_int_interval,
        .ctx = &s_staging_config,
    },
    {
        .id = "target_temperature",
        .title = "Target Temperature",
        .group = "climate",
        .unit = "\xC2\xB0" "C",
        .type = DEVICE_SETTING_INT,
        .flags = 0,
        .min_value = 30,
        .max_value = 120,
        .step = 1,
        .read = read_int_temp,
        .stage = stage_int_temp,
        .ctx = &s_staging_config,
    },
    {
        .id = "fan_mode",
        .title = "Fan Mode",
        .group = "climate",
        .unit = "",
        .type = DEVICE_SETTING_ENUM,
        .flags = 0,
        .options = s_fan_options,
        .option_count = 3,
        .read = read_enum_fan,
        .stage = stage_enum_fan,
        .ctx = &s_staging_config,
    },
    {
        .id = "device_label",
        .title = "Device Label",
        .group = "",
        .unit = "",
        .type = DEVICE_SETTING_STRING,
        .flags = 0,
        .max_length = 32,
        .read = read_string_label,
        .stage = stage_string_label,
        .ctx = &s_staging_config,
    },
    {
        .id = "admin_token",
        .title = "Admin Token",
        .group = "security",
        .unit = "",
        .type = DEVICE_SETTING_STRING,
        .flags = DEVICE_SETTING_FLAG_SECRET,
        .max_length = 32,
        .read = read_secret_token,
        .stage = stage_secret_token,
        .ctx = &s_staging_config,
    },
    {
        .id = "serial_number",
        .title = "Serial Number",
        .group = "",
        .unit = "",
        .type = DEVICE_SETTING_STRING,
        .flags = DEVICE_SETTING_FLAG_READONLY,
        .max_length = 16,
        .read = read_string_serial,
        .stage = NULL,
        .ctx = &s_staging_config,
    },
};

#define SETTINGS_COUNT (sizeof(s_settings) / sizeof(s_settings[0]))

/* ------------------------------------------------------------------ *
 * Cross-field validation
 * ------------------------------------------------------------------ */

esp_err_t reference_config_validate(const void *config, size_t config_size)
{
    if (config == NULL || config_size < sizeof(reference_config_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const reference_config_t *cfg = (const reference_config_t *)config;

    /* Rule: if sensor is disabled, sample_interval is meaningless.
     * Enforce sample_interval == 1 (minimum) when sensor off. */
    if (!cfg->sensor_enabled && cfg->sample_interval < 1) {
        ESP_LOGW(TAG, "validation: sensor disabled but interval < 1");
        return ESP_ERR_INVALID_ARG;
    }

    /* Rule: target_temperature must be even (demo constraint). */
    if (cfg->target_temperature % 2 != 0) {
        ESP_LOGW(TAG, "validation: target_temperature must be even");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 * Registration
 * ------------------------------------------------------------------ */

int reference_config_register_settings(void)
{
    for (size_t i = 0; i < SETTINGS_COUNT; i++) {
        esp_err_t err = device_settings_register(&s_settings[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register failed for '%s': %s",
                     s_settings[i].id, esp_err_to_name(err));
            return -1;
        }
    }

    /* Set validation callback */
    device_settings_set_validate_fn(reference_config_validate);

    ESP_LOGI(TAG, "registered %d settings", (int)SETTINGS_COUNT);
    return 0;
}

/* ------------------------------------------------------------------ *
 * Config accessors (for boot defaults + test access)
 * ------------------------------------------------------------------ */

reference_config_t *reference_config_get_active(void)
{
    return &s_active_config;
}

reference_config_t *reference_config_get_staging(void)
{
    return &s_staging_config;
}

void reference_config_apply_defaults(reference_config_t *cfg)
{
    if (cfg == NULL) return;
    cfg->header.format_version = 1;
    cfg->header.config_revision = 1;
    cfg->sensor_enabled = REFERENCE_CONFIG_DEFAULT_SENSOR_ENABLED;
    cfg->sample_interval = REFERENCE_CONFIG_DEFAULT_SAMPLE_INTERVAL;
    cfg->target_temperature = REFERENCE_CONFIG_DEFAULT_TARGET_TEMP;
    cfg->fan_mode = REFERENCE_CONFIG_DEFAULT_FAN_MODE;
    strlcpy(cfg->device_label, REFERENCE_CONFIG_DEFAULT_DEVICE_LABEL,
            sizeof(cfg->device_label));
    cfg->admin_token_set = false;
    memset(cfg->admin_token_hash, 0, sizeof(cfg->admin_token_hash));
    strlcpy(cfg->serial_number, REFERENCE_CONFIG_DEFAULT_SERIAL_NUMBER,
            sizeof(cfg->serial_number));
}
