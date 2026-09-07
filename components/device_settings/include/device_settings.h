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

typedef struct {
    uint8_t index;
} device_setting_enum_value_t;

/* Typed callback ABI. A descriptor selects the member matching its type. */
typedef union {
    esp_err_t (*read_bool)(void *ctx, bool *out);
    esp_err_t (*read_int)(void *ctx, int32_t *out);
    esp_err_t (*read_string)(void *ctx, char *out, size_t out_cap);
    esp_err_t (*read_enum)(void *ctx, device_setting_enum_value_t *out);
    esp_err_t (*read)(void *ctx, void *out); /* legacy */
} device_setting_read_cb_t;

typedef union {
    esp_err_t (*stage_bool)(void *ctx, bool value);
    esp_err_t (*stage_int)(void *ctx, int32_t value);
    esp_err_t (*stage_string)(void *ctx, const char *value, size_t value_len);
    esp_err_t (*stage_enum)(void *ctx, device_setting_enum_value_t value);
    esp_err_t (*stage)(void *ctx, const void *value); /* legacy */
} device_setting_stage_cb_t;

/* ------------------------------------------------------------------ *
 * Setting descriptor (static const in flash)
 * ------------------------------------------------------------------ */

typedef struct device_setting_descriptor {
    const char *id;
    const char *title;
    const char *group;
    const char *unit;
    device_setting_type_t type;
    uint16_t flags;
    int32_t min_value;
    int32_t max_value;
    int32_t step;
    uint16_t max_length;
    const device_setting_option_t *options;
    uint8_t option_count;
    device_setting_read_cb_t read_cb;
    device_setting_stage_cb_t stage_cb;
    esp_err_t (*read)(void *ctx, void *out); /* compatibility ABI */
    esp_err_t (*stage)(void *ctx, const void *value); /* compatibility ABI */
    void *read_ctx;
    void *stage_ctx;
    void *ctx; /* compatibility context */
} device_setting_descriptor_t;

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

typedef esp_err_t (*device_settings_defaults_fn)(
    void *payload, size_t payload_size);

typedef esp_err_t (*device_settings_validate_fn)(
    const void *config, size_t config_size);

/* Product-owned static storage bound once per boot after init. */
typedef struct {
    size_t config_size;
    uint16_t format_version;
    void *active_config;
    void *staging_config;
    device_settings_defaults_fn defaults_fn;
    device_settings_validate_fn validate_fn;
} device_settings_storage_config_t;

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
 * Load result
 * ------------------------------------------------------------------ */

typedef enum {
    DEVICE_SETTINGS_LOAD_OK = 0,
    DEVICE_SETTINGS_LOAD_DEFAULTS_NOT_FOUND,
    DEVICE_SETTINGS_LOAD_DEFAULTS_RECOVERED,
    DEVICE_SETTINGS_LOAD_FATAL,
} device_settings_load_result_t;

/* ------------------------------------------------------------------ *
 * Public API — Registry
 * ------------------------------------------------------------------ */

/* Initialize runtime state. Storage must be configured separately. */
esp_err_t device_settings_init(void);

/* Bind product-owned active/staging buffers and persisted format metadata.
 * Must be called exactly once after init and before registration/freeze. */
esp_err_t device_settings_configure(
    const device_settings_storage_config_t *config);

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

/* Set a single setting value with an explicit transaction ID. */
esp_err_t device_settings_tx_set_with_id(uint64_t transaction_id,
                                         const char *setting_id,
                                         device_setting_type_t type,
                                         const void *value);

/* Legacy wrapper; new command paths must use tx_set_with_id(). */
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

/* Validate commit confirmation and transition to restart-pending.
 * Restart orchestration is owned by device_command/device_app. */
esp_err_t device_settings_tx_validate_confirm(uint64_t transaction_id,
                                              uint32_t revision);
esp_err_t device_settings_tx_confirm(uint64_t transaction_id,
                                     uint32_t revision);

/* Legacy wrapper; validates the last committed transaction. */
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

/* Load config from NVS or product defaults and report recovery outcome. */
esp_err_t device_settings_load(device_settings_load_result_t *out_result);

/* Save config to NVS atomically.
 * Internal use only — called by tx_commit. */
esp_err_t device_settings_save(const void *config, size_t config_size,
                               uint32_t new_revision);

/* ------------------------------------------------------------------ *
 * Public API — Product integration
 * ------------------------------------------------------------------ */

/* Legacy callback setters; new products bind callbacks in storage config. */
void device_settings_set_validate_fn(device_settings_validate_fn fn);
void device_settings_set_defaults_fn(device_settings_defaults_fn fn);

/* Legacy storage setters are internal compatibility helpers. New products
 * must use device_settings_configure(). */
void device_settings_set_config_size(size_t size);
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
void device_settings_tx_arm_confirm_timeout(void);

/* Confirm timeout callback for use with FreeRTOS timer creation. */
void device_settings_confirm_timeout_cb(void *arg);

/* Reset transaction state to IDLE. Used by device_settings_init(). */
void device_settings_tx_reset_state(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_SETTINGS_H */