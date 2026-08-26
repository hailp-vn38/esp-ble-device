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
 *   device_id = logical identity
 *   bool_value = success/failure
 *   int_value = result/state
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

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

int device_command_init(int (*notify_fn)(const uint8_t *, size_t));
int device_command_register(const char *command, device_cmd_handler_t handler);
int device_command_freeze(void);
int device_command_submit(const uint8_t *data, size_t len);
int device_command_complete(const gw_message_t *request,
                            const device_cmd_response_t *response);

/* Set the logical device_id used in ACK echo. */
void device_command_set_device_id(const char *id);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_COMMAND_H */
