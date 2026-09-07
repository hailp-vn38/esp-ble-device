/*
 * device_settings — Settings v2 core, registry, validation & persistence.
 *
 * Core module (Phase 1) for device-side settings management.
 * Independent of BLE transport.
 *
 * Architecture:
 *   - Descriptors are static const in flash; registry holds pointers only.
 *   - Active/staging config model with atomic NVS commit.
 *   - Transaction model: BEGIN -> SET* -> COMMIT -> COMMIT_CONFIRM -> reboot.
 *   - config_revision is persisted atomically within the config blob.
 */
#ifndef DEVICE_SETTINGS_H
#define DEVICE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Constants
 * ------------------------------------------------------------------ */

#define DEVICE_SETTING_MAX_COUNT        12
#define DEVICE_SETTING_ID_MAX_LEN       32
#define DEVICE_SETTING_TITLE_MAX_LEN    48
#define DEVICE_SETTING_GROUP_MAX_LEN    32
#define DEVICE_SETTING_UNIT_MAX_LEN     16
#define DEVICE_SETTING_STRING_MAX_LEN   64
#define DEVICE_SETTING_ENUM_MAX_OPTIONS 8
#define DEVICE_SETTING_TX_TIMEOUT_MS    10000

/* ------------------------------------------------------------------ *
 * Setting types and flags
 * ------------------------------------------------------------------ */

typedef enum {
    DEVICE_SETTING_BOOL   = 0,
    DEVICE_SETTING_INT    = 1,
    DEVICE_SETTING_STRING = 2,
    DEVICE_SETTING_ENUM   = 3,
} device_setting_type_t;

enum {
    DEVICE_SETTING_FLAG_READONLY  = 1u << 0,
    DEVICE_SETTING_FLAG_SECRET    = 1u << 1,
    DEVICE_SETTING_FLAG_ADVANCED  = 1u << 2,
};

/* ------------------------------------------------------------------ *
 * Enum option descriptor
 * ------------------------------------------------------------------ */

typedef struct {
    const char *label;
} device_setting_option_t;

/* ------------------------------------------------------------------ *
 * Setting descriptor (static const in flash)
 * ------------------------------------------------------------------ */

typedef struct device_setting_descriptor {
    const char *id;         /* Unique identifier (e.g. "wifi_ssid") */
    const char *title;      /* Human-readable title (e.g. "WiFi SSID") */
    const char *group;      /* Group name (e.g. "network") or "" */
    const char *unit;       /* Unit string (e.g. "dBm") or "" */
    device_setting_type_t type;
    uint16_t flags;         /* DEVICE_SETTING_FLAG_* */
    int32_t min_value;      /* For INT: minimum value */
    int32_t max_value;      /* For INT: maximum value */
    int32_t step;           /* For INT: step size */
    uint16_t max_length;    /* For STRING: max character count */
    const device_setting_option_t *options;  /* For ENUM: option array */
    uint8_t option_count;   /* For ENUM: number of options */
    esp_err_t (*read)(void *ctx, void *out);     /* Read current value */
    esp_err_t (*stage)(void *ctx, const void *value);  /* Stage new value */
    void *ctx;              /* User context for read/stage callbacks */
} device_setting_descriptor_t;

/* ------------------------------------------------------------------ *
 * Enum option value (for read/stage callbacks)
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t index;
} device_setting_enum_value_t;

/* ------------------------------------------------------------------ *
 * Secret value (for read callback)
 * ------------------------------------------------------------------ */

typedef struct {
    bool configured;
    /* value is NEVER returned in plaintext */
} device_setting_secret_value_t;

/* ------------------------------------------------------------------ *
 * Config blob header (persisted atomically)
 * ------------------------------------------------------------------ */

typedef struct {
    uint16_t format_version;
    uint16_t reserved;
    uint32_t config_revision;
} device_settings_blob_header_t;

/* ------------------------------------------------------------------ *
 * Transaction state
 * ------------------------------------------------------------------ */

typedef enum {
    DEVICE_SETTINGS_TX_IDLE = 0,
    DEVICE_SETTINGS_TX_ACTIVE = 1,
    DEVICE_SETTINGS_TX_COMMITTED_WAIT_CONFIRM = 2,
    DEVICE_SETTINGS_TX_RESTART_PENDING = 3,
} device_settings_tx_state_t;

/* ------------------------------------------------------------------ *
 * Validation callback (product-level cross-field validation)
 * ------------------------------------------------------------------ */

typedef esp_err_t (*device_settings_validate_fn)(
    const void *config, size_t config_size);

/* ------------------------------------------------------------------ *
 * Public API — Registry
 * ------------------------------------------------------------------ */

/* Initialize the settings module. Must be called before register. */
esp_err_t device_settings_init(void);

/* Register a setting descriptor. Only valid before freeze.
 * Returns ESP_ERR_INVALID_STATE if already frozen. */
esp_err_t device_settings_register(const device_setting_descriptor_t *desc);

/* Freeze the registry. No more registrations allowed after this. */
esp_err_t device_settings_freeze(void);

/* Get the number of registered settings. */
size_t device_settings_count(void);

/* Get a setting descriptor by index. */
const device_setting_descriptor_t *device_settings_get(size_t index);

/* Find a setting descriptor by ID. */
const device_setting_descriptor_t *device_settings_find(const char *id);

/* Get current config revision. */
uint32_t device_settings_get_revision(void);

/* ------------------------------------------------------------------ *
 * Public API — Transaction
 * ------------------------------------------------------------------ */

/* Begin a new transaction. Validates expected_revision against current.
 * Returns ESP_ERR_INVALID_STATE if another transaction is active.
 * Returns ESP_ERR_INVALID_VERSION if expected_revision doesn't match. */
esp_err_t device_settings_tx_begin(uint64_t transaction_id,
                                   uint32_t expected_revision);

/* Set a single setting value during an active transaction.
 * Validates type, range, and calls the stage callback.
 * Returns ESP_ERR_INVALID_STATE if no transaction is active. */
esp_err_t device_settings_tx_set(const char *setting_id,
                                 device_setting_type_t type,
                                 const void *value);

/* Commit all staged changes atomically to NVS.
 * Validates all fields and cross-field rules.
 * Promotes staging to active only after NVS commit success.
 * Returns ESP_ERR_INVALID_STATE if no transaction is active. */
esp_err_t device_settings_tx_commit(uint64_t transaction_id,
                                    uint32_t *new_revision);

/* Abort the current transaction. Discards all staged changes.
 * Safe to call even if no transaction is active. */
esp_err_t device_settings_tx_abort(uint64_t transaction_id);

/* Get the current transaction state. */
device_settings_tx_state_t device_settings_tx_get_state(void);

/* Confirm commit and trigger reboot.
 * Must be called after COMMITTED_WAIT_CONFIRM state. */
esp_err_t device_settings_tx_confirm_and_restart(void);

/* Called by BLE layer on disconnect. If in COMMITTED_WAIT_CONFIRM,
 * schedules restart immediately (no rollback of persisted config). */
void device_settings_tx_on_disconnect(void);

/* Get the last committed transaction ID and revision (idempotency).
 * Returns true if a commit has occurred in this boot cycle. */
bool device_settings_tx_get_last_committed(uint64_t *out_tx_id,
                                           uint32_t *out_revision);

/* ------------------------------------------------------------------ *
 * Public API — Persistence
 * ------------------------------------------------------------------ */

/* Load config from NVS. If no config exists, uses defaults.
 * Returns ESP_OK on success, ESP_ERR_NOT_FOUND if no config saved. */
esp_err_t device_settings_load(void);

/* Save config to NVS atomically.
 * Internal use only — called by tx_commit. */
esp_err_t device_settings_save(const void *config, size_t config_size,
                               uint32_t new_revision);

/* ------------------------------------------------------------------ *
 * Public API — Product integration
 * ------------------------------------------------------------------ */

/* Set the product-level validation callback. */
void device_settings_set_validate_fn(device_settings_validate_fn fn);

/* Set the config blob size (must be called before init). */
void device_settings_set_config_size(size_t size);

/* Set the format version (must be called before init). */
void device_settings_set_format_version(uint16_t version);

/* Get pointer to active config blob (header + product fields). */
const void *device_settings_get_active_config(void);

/* Get pointer to staging config blob. */
void *device_settings_get_staging_config(void);

/* ------------------------------------------------------------------ *
 * Secret handling
 * ------------------------------------------------------------------ */

/* Write action for secret values. */
typedef enum {
    DEVICE_SETTING_SECRET_KEEP = 0,
    DEVICE_SETTING_SECRET_SET  = 1,
    DEVICE_SETTING_SECRET_CLEAR = 2,
} device_setting_secret_action_t;

/* Set the confirm timeout timer handle (created by device_app). */
void device_settings_tx_set_confirm_timer(void *timer);

/* Confirm timeout callback for use with FreeRTOS timer creation. */
void device_settings_confirm_timeout_cb(void *arg);

/* Reset transaction state to IDLE. Used by device_settings_init(). */
void device_settings_tx_reset_state(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_SETTINGS_H */