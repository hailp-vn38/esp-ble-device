/*
 * device_command — command RX pipeline (docs §23..#31).
 *
 * Pipeline:
 *   ABF1 write
 *     -> BLE RX queue (bounded copy from ble_peripheral)
 *     -> command worker task
 *       -> CBOR decode (gw_message_decode)
 *       -> validate (version, type, required fields)
 *       -> lookup handler
 *       -> execute handler / accept long-running
 *       -> build ACK (gw_build_ack)
 *       -> encode + notify via ABF2
 *
 * ACK contract:
 *   type = device_ack
 *   request_id = exact echo
 *   command = exact echo
 *   device_id = exact request->device_id (Gateway routing identity, NOT native model)
 *   bool_value = success/failure
 *   int_value = result/state
 *
 * Routing identity rules (spec D2, D3):
 *   - ACK always echoes request->device_id (Gateway-assigned routing ID)
 *   - Native model (e.g. "esp32s3-ref") is metadata only, never used for ACK routing
 *   - device_command_set_device_id() is DEPRECATED and should not be called
 *
 * Capability registry rules (spec D5, D6, D7, D8):
 *   - Registry order is DETERMINISTIC and treated as PRESENTATION ORDER (spec D8)
 *   - Built-in commands (ping, get_info) are INTERNAL by default (spec D6)
 *   - get_state is INTERNAL by default but can be PROMOTED by product (spec D6)
 *   - Capability revision must bump when public metadata changes (spec D7)
 *   - Capability order is frozen after device_command_freeze() (spec D8)
 *   - Max public capabilities: 12 (spec §4.6)
 */
#ifndef DEVICE_COMMAND_H
#define DEVICE_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gateway_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Command handler
 * ------------------------------------------------------------------ */

typedef enum {
    DEVICE_CMD_OK = 0,
    DEVICE_CMD_ERR_HANDLER = -1,
    DEVICE_CMD_ERR_UNKNOWN = -2,
    DEVICE_CMD_ERR_VALIDATION = -3,
    DEVICE_CMD_ERR_BUSY = -4,
} device_cmd_result_t;

typedef struct {
    bool success;
    int int_value;
    bool long_running;
} device_cmd_response_t;

typedef device_cmd_result_t (*device_cmd_handler_t)(
    const gw_message_t *request,
    device_cmd_response_t *response);

#define DEVICE_COMMAND_MAX_CAPABILITIES 12

typedef enum {
    DEVICE_CMD_VALUE_NONE = 0,
    DEVICE_CMD_VALUE_BOOL = 1,
    DEVICE_CMD_VALUE_INT = 2,
} device_cmd_value_type_t;

enum {
    DEVICE_CMD_FLAG_IDEMPOTENT = 1u << 0,
    DEVICE_CMD_FLAG_DESTRUCTIVE = 1u << 1,
};

/* ------------------------------------------------------------------ *
 * Capability registration (spec D5, D6, D7, D8)
 *
 * Max public capabilities: 12 (spec §4.6)
 * Value types: NONE (0), BOOL (1), INT (2) only (spec §12)
 * Flags: IDEMPOTENT (1<<0), DESTRUCTIVE (1<<1)
 *
 * Registration order is DETERMINISTIC and becomes PRESENTATION ORDER (spec D8).
 * Built-in commands (ping, get_info) are INTERNAL by default (spec D6).
 * get_state is INTERNAL by default but can be PROMOTED by product (spec D6).
 * ------------------------------------------------------------------ */

typedef struct {
    const char *command;
    const char *label;
    const char *unit;
    device_cmd_value_type_t value_type;
    uint8_t flags;
    int32_t min_value;
    int32_t max_value;
    uint32_t step;
} device_cmd_capability_t;

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

int device_command_init(int (*notify_fn)(const uint8_t *, size_t));
int device_command_register(const char *command, device_cmd_handler_t handler);
/* Register or override a handler and expose its argument contract to gateway
 * capability discovery. Metadata is copied during the call. */
int device_command_register_capability(
    const device_cmd_capability_t *capability,
    device_cmd_handler_t handler);
int device_command_freeze(void);
int device_command_submit(const uint8_t *data, size_t len);
int device_command_complete(const gw_message_t *request,
                            const device_cmd_response_t *response);

/* DEPRECATED: Do not use. ACK routing identity is always request->device_id.
 * This function exists for backward compatibility only. Calling it with a
 * model-derived ID would corrupt ACK routing identity (spec D3). */
void device_command_set_device_id(const char *id) __attribute__((deprecated("ACK routing uses request->device_id; do not call")));

/* Set the capability revision for this device (spec D7).
 * Must be called before device_command_freeze().
 * Must increment when public capability schema changes (spec D7). */
void device_command_set_capability_revision(uint32_t revision);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_COMMAND_H */
