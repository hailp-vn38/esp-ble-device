#ifndef DEMO_CONFIG_H
#define DEMO_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "device_settings.h"

typedef enum {
    DEMO_FAN_MODE_OFF = 0,
    DEMO_FAN_MODE_MANUAL = 1,
    DEMO_FAN_MODE_AUTO = 2,
} demo_fan_mode_t;

typedef struct {
    device_settings_blob_header_t header;
    bool sensor_enabled;
    int32_t sample_interval_s;
    int32_t startup_dryer_temp_c;
    uint8_t startup_fan_mode;
    int32_t startup_fan_percent;
    char device_label[33];
    bool diagnostic_logging;
    char serial_number[17];
} demo_config_t;

extern demo_config_t g_demo_active_config;
extern demo_config_t g_demo_staging_config;

int demo_config_register_settings(void);
esp_err_t demo_config_validate(const void *config, size_t size);
esp_err_t demo_config_defaults(void *payload, size_t payload_size);

#endif
