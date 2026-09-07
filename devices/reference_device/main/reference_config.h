/*
 * reference_config — Config blob for Reference Device (Phase 5).
 *
 * Defines the persisted config structure with 7 demo settings
 * covering all types: BOOL, INT, STRING, ENUM, SECRET, READONLY.
 *
 * Config is versioned with format_version for migration support.
 */
#ifndef REFERENCE_CONFIG_H
#define REFERENCE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "device_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Config blob (persisted atomically to NVS)
 * ------------------------------------------------------------------ */

typedef struct {
    device_settings_blob_header_t header;

    /* Writable settings */
    bool sensor_enabled;        /* BOOL — sensor on/off */
    int32_t sample_interval;    /* INT — 1..3600 s */
    int32_t target_temperature; /* INT — 30..120 °C */
    int32_t fan_mode;           /* ENUM — Off=0, Manual=1, Auto=2 */
    char device_label[33];      /* STRING — max 32 chars + null */

    /* Secret setting (never read back in plaintext) */
    bool admin_token_set;       /* true if a token has been configured */
    char admin_token_hash[33];  /* Simulated hash of admin token */

    /* Readonly setting */
    char serial_number[17];     /* READONLY — fixed at production */
} reference_config_t;

/* ------------------------------------------------------------------ *
 * Settings registration
 * ------------------------------------------------------------------ */

/* Register all settings with the device_settings registry.
 * Must be called after device_settings_init() and before freeze.
 * Returns 0 on success, -1 on error. */
int reference_config_register_settings(void);

/* Product-level cross-field validation.
 * Called before NVS commit to ensure consistency.
 * Example: if sensor is disabled, sample_interval is irrelevant. */
esp_err_t reference_config_validate(const void *config, size_t config_size);

/* Access to active/staging config instances (for tests). */
reference_config_t *reference_config_get_active(void);
reference_config_t *reference_config_get_staging(void);

/* Apply factory defaults to a config blob. */
void reference_config_apply_defaults(reference_config_t *cfg);

/* ------------------------------------------------------------------ *
 * Defaults
 * ------------------------------------------------------------------ */

#define REFERENCE_CONFIG_DEFAULT_SENSOR_ENABLED    true
#define REFERENCE_CONFIG_DEFAULT_SAMPLE_INTERVAL   10
#define REFERENCE_CONFIG_DEFAULT_TARGET_TEMP       60
#define REFERENCE_CONFIG_DEFAULT_FAN_MODE          2  /* Auto */
#define REFERENCE_CONFIG_DEFAULT_DEVICE_LABEL      "RefDevice"
#define REFERENCE_CONFIG_DEFAULT_SERIAL_NUMBER     "REF-0001"

#ifdef __cplusplus
}
#endif

#endif /* REFERENCE_CONFIG_H */
