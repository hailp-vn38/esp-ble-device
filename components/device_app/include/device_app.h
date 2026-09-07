/*
 * device_app — composition root (docs §9..#12, §86..#90).
 *
 * Boot sequence (doc §86, critical):
 *   1. logging
 *   2. NVS init
 *   3. device_storage init
 *   4. device_core init
 *   5. board_io init
 *   6. product init
 *   7. gateway_protocol init/check
 *   8. device_command init
 *   9. register common commands
 *  10. register product commands
 *  11. freeze command registry
 *  12. device_event init
 *  13. product event registration
 *  14. ble_peripheral init
 *  15. start product
 *  16. start advertising
 *
 * Depends: gateway_protocol, ble_peripheral, device_command, device_event.
 * Does NOT: NimBLE, CBOR, GPIO directly.
 *
 * Routing identity rules (spec D2, D3, D4):
 *   - profile.model is NATIVE identity (e.g. "esp32s3-ref")
 *   - Native identity is for events/metadata only
 *   - ACK/capability routing always uses request->device_id from Gateway
 *   - device_command_set_device_id() is DEPRECATED and NOT called
 */
#ifndef DEVICE_APP_H
#define DEVICE_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "gateway_protocol.h"
#include "ble_peripheral.h"
#include "device_command.h"
#include "device_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Result codes
 * ------------------------------------------------------------------ */

typedef enum {
    DEVICE_APP_OK = 0,
    DEVICE_APP_ERR_INVALID_ARG = -1,
    DEVICE_APP_ERR_INVALID_STATE = -2,
    DEVICE_APP_ERR_STORAGE = -3,
    DEVICE_APP_ERR_PROTOCOL = -4,
    DEVICE_APP_ERR_COMMAND = -5,
    DEVICE_APP_ERR_EVENT = -6,
    DEVICE_APP_ERR_BLE = -7,
    DEVICE_APP_ERR_PRODUCT = -8,
    DEVICE_APP_ERR_NO_RESOURCE = -9,
} device_app_result_t;

/* ------------------------------------------------------------------ *
 * Product profile (docs §51, §11)
 *
 * Routing identity rules (spec D2, D3, D4):
 *   - model: NATIVE identity (e.g. "esp32s3-ref"), used for events only
 *   - NOT used for ACK routing (that uses request->device_id from Gateway)
 *   - device_command_set_device_id() is DEPRECATED and NOT called
 * ------------------------------------------------------------------ */

typedef struct {
    const char *model;           /* Native identity, NOT Gateway routing ID */
    const char *hardware_version;
    const char *firmware_version;
    const char *ble_name_prefix;
    uint8_t protocol_version;
    uint32_t capability_revision;
    bool supports_factory_reset;
    bool supports_telemetry;
    bool supports_local_button;

    /* Lifecycle callbacks */
    int (*product_init)(void);
    int (*product_start)(void);
    int (*product_stop)(void);

    /* Command/event registration */
    int (*register_commands)(void);
    int (*register_features)(void);
    int (*register_events)(void);

    /* Settings v2 registration */
    int (*register_settings)(void);
    uint32_t settings_schema_revision;
    uint16_t settings_format_version;
    size_t settings_config_size;
    void *settings_active_config;
    void *settings_staging_config;
} device_app_profile_t;

/* ------------------------------------------------------------------ *
 * Status
 * ------------------------------------------------------------------ */

typedef struct {
    ble_peripheral_state_t ble_state;
    bool ready;
    uint16_t mtu;
    bool bonded;
} device_app_status_t;

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

/* Set the product profile. Must be called before device_app_start(). */
device_app_result_t device_app_set_profile(const device_app_profile_t *profile);

/* Start the device (full boot sequence per doc §86). */
device_app_result_t device_app_start(void);

/* Stop the device gracefully (doc §88). */
device_app_result_t device_app_stop(void);

/* Get current device status. */
device_app_result_t device_app_get_status(device_app_status_t *out_status);

/* Factory reset: clear bonds + resettable state (doc §90). */
device_app_result_t device_app_factory_reset(void);

/* Schedule a safe restart after delay_ms.
 * Does NOT call esp_restart() directly — uses a FreeRTOS timer so the
 * current BLE callback / notify task can unwind first.
 * Multiple calls reset the timer to the latest delay. */
device_app_result_t device_app_schedule_restart(uint32_t delay_ms);

/* Cancel a previously scheduled restart (e.g. if new TX begins). */
device_app_result_t device_app_cancel_restart(void);

/* Check if a restart is currently scheduled. */
bool device_app_is_restart_scheduled(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_APP_H */
