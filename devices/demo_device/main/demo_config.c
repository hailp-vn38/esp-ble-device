#include "demo_config.h"

#include <string.h>

 demo_config_t g_demo_active_config;
 demo_config_t g_demo_staging_config;

static esp_err_t read_bool(void *ctx, void *out)
{
    *(bool *)out = *(const bool *)ctx;
    return ESP_OK;
}

static esp_err_t read_int(void *ctx, void *out)
{
    *(int32_t *)out = *(const int32_t *)ctx;
    return ESP_OK;
}

static esp_err_t read_enum(void *ctx, void *out)
{
    *(uint8_t *)out = *(const uint8_t *)ctx;
    return ESP_OK;
}

static esp_err_t read_string(void *ctx, void *out)
{
    strncpy((char *)out, (const char *)ctx, DEVICE_SETTING_STRING_MAX_LEN - 1);
    ((char *)out)[DEVICE_SETTING_STRING_MAX_LEN - 1] = '\0';
    return ESP_OK;
}

static esp_err_t stage_bool(void *ctx, const void *value)
{
    *(bool *)ctx = *(const bool *)value;
    return ESP_OK;
}

static esp_err_t stage_int(void *ctx, const void *value)
{
    *(int32_t *)ctx = *(const int32_t *)value;
    return ESP_OK;
}

static esp_err_t stage_enum(void *ctx, const void *value)
{
    *(uint8_t *)ctx = *(const uint8_t *)value;
    return ESP_OK;
}

static esp_err_t stage_string(void *ctx, const void *value)
{
    strncpy((char *)ctx, (const char *)value, 32);
    ((char *)ctx)[32] = '\0';
    return ESP_OK;
}

static const device_setting_option_t fan_options[] = {
    { .label = "Off" },
    { .label = "Manual" },
    { .label = "Auto" },
};

#define COMMON(ID, TITLE, TYPE, FLAGS, MIN, MAX, STEP, LEN, READ, STAGE, READ_CTX, STAGE_CTX) \
    { .id = ID, .title = TITLE, .group = "demo", .unit = "", .type = TYPE, \
      .flags = FLAGS, .min_value = MIN, .max_value = MAX, .step = STEP, \
      .max_length = LEN, .read = READ, .stage = STAGE, .read_ctx = READ_CTX, \
      .stage_ctx = STAGE_CTX }
#define ENUM_SETTING(ID, TITLE, READ_CTX, STAGE_CTX) \
    { .id = ID, .title = TITLE, .group = "demo", .unit = "", .type = DEVICE_SETTING_ENUM, \
      .options = fan_options, .option_count = 3, .read = read_enum, .stage = stage_enum, \
      .read_ctx = READ_CTX, .stage_ctx = STAGE_CTX }

static const device_setting_descriptor_t settings[] = {
    COMMON("sensor_enabled", "Sensor Enabled", DEVICE_SETTING_BOOL, 0, 0, 0, 0, 0,
           read_bool, stage_bool, &g_demo_active_config.sensor_enabled,
           &g_demo_staging_config.sensor_enabled),
    COMMON("sample_interval_s", "Sample Interval", DEVICE_SETTING_INT, 0, 1, 60, 1, 0,
           read_int, stage_int, &g_demo_active_config.sample_interval_s,
           &g_demo_staging_config.sample_interval_s),
    COMMON("startup_dryer_temp_c", "Startup Dryer Temperature", DEVICE_SETTING_INT,
           0, 30, 100, 1, 0, read_int, stage_int,
           &g_demo_active_config.startup_dryer_temp_c,
           &g_demo_staging_config.startup_dryer_temp_c),
    ENUM_SETTING("startup_fan_mode", "Startup Fan Mode",
                 &g_demo_active_config.startup_fan_mode,
                 &g_demo_staging_config.startup_fan_mode),
    COMMON("startup_fan_percent", "Startup Fan Percent", DEVICE_SETTING_INT, 0, 0, 100, 1, 0,
           read_int, stage_int, &g_demo_active_config.startup_fan_percent,
           &g_demo_staging_config.startup_fan_percent),
    COMMON("device_label", "Device Label", DEVICE_SETTING_STRING, 0, 0, 0, 0, 32,
           read_string, stage_string, g_demo_active_config.device_label,
           g_demo_staging_config.device_label),
    COMMON("diagnostic_logging", "Diagnostic Logging", DEVICE_SETTING_BOOL,
           DEVICE_SETTING_FLAG_ADVANCED, 0, 0, 0, 0, read_bool, stage_bool,
           &g_demo_active_config.diagnostic_logging,
           &g_demo_staging_config.diagnostic_logging),
    COMMON("serial_number", "Serial Number", DEVICE_SETTING_STRING,
           DEVICE_SETTING_FLAG_READONLY, 0, 0, 0, 16, read_string, NULL,
           g_demo_active_config.serial_number, NULL),
};

#undef ENUM_SETTING
#undef COMMON

int demo_config_register_settings(void)
{
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
        if (device_settings_register(&settings[i]) != ESP_OK) return -1;
    }
    return 0;
}

esp_err_t demo_config_defaults(void *payload, size_t payload_size)
{
    if (payload == NULL || payload_size != sizeof(demo_config_t) -
        sizeof(device_settings_blob_header_t)) return ESP_ERR_INVALID_ARG;
    demo_config_t defaults = {0};
    defaults.sensor_enabled = true;
    defaults.sample_interval_s = 3;
    defaults.startup_dryer_temp_c = 60;
    defaults.startup_fan_mode = DEMO_FAN_MODE_AUTO;
    defaults.startup_fan_percent = 50;
    strcpy(defaults.device_label, "Demo Device");
    strcpy(defaults.serial_number, "DEMO-0001");
    memcpy(payload, (const uint8_t *)&defaults + sizeof(defaults.header), payload_size);
    return ESP_OK;
}

esp_err_t demo_config_validate(const void *config, size_t size)
{
    const demo_config_t *cfg = (const demo_config_t *)config;
    if (cfg == NULL || size != sizeof(*cfg) ||
        cfg->sample_interval_s < 1 || cfg->sample_interval_s > 60 ||
        cfg->startup_dryer_temp_c < 30 || cfg->startup_dryer_temp_c > 100 ||
        cfg->startup_fan_mode > DEMO_FAN_MODE_AUTO ||
        cfg->startup_fan_percent < 0 || cfg->startup_fan_percent > 100 ||
        (cfg->startup_fan_mode == DEMO_FAN_MODE_MANUAL &&
         cfg->startup_fan_percent < 10)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
