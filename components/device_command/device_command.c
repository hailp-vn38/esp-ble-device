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
#include "device_command.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "gateway_protocol.h"

static const char *TAG = "device_command";

/* ------------------------------------------------------------------ *
 * Shared RX message type (same layout as ble_peripheral rx_msg_t)
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t data[GW_MSG_MAX_LEN];
    uint16_t len;
} cmd_rx_msg_t;

/* ------------------------------------------------------------------ *
 * Constants
 * ------------------------------------------------------------------ */

#define CMD_REGISTRY_MAX     32
#define CMD_TASK_STACK       4096
#define CMD_TASK_PRIORITY    4
#define CMD_RX_QUEUE_DEPTH   8

/* ------------------------------------------------------------------ *
 * Registry entry
 * ------------------------------------------------------------------ */

typedef struct {
    char command[GW_MSG_COMMAND_LEN];
    device_cmd_handler_t handler;
} cmd_entry_t;

/* ------------------------------------------------------------------ *
 * Internal state
 * ------------------------------------------------------------------ */

static struct {
    cmd_entry_t registry[CMD_REGISTRY_MAX];
    int registry_count;
    bool frozen;

    QueueHandle_t rx_queue;
    TaskHandle_t worker_task;

    int (*notify_fn)(const uint8_t *, size_t);

    char device_id[GW_MSG_DEVICE_ID_LEN];
    bool has_device_id;
} s_cmd;

/* ------------------------------------------------------------------ *
 * Forward declarations (built-in commands, doc §27)
 * ------------------------------------------------------------------ */

static device_cmd_result_t cmd_ping_handler(
    const gw_message_t *request, device_cmd_response_t *response);
static device_cmd_result_t cmd_get_info_handler(
    const gw_message_t *request, device_cmd_response_t *response);
static device_cmd_result_t cmd_get_state_handler(
    const gw_message_t *request, device_cmd_response_t *response);

/* ------------------------------------------------------------------ *
 * Registry lookup
 * ------------------------------------------------------------------ */

static device_cmd_handler_t find_handler(const char *command)
{
    for (int i = 0; i < s_cmd.registry_count; i++) {
        if (strcmp(s_cmd.registry[i].command, command) == 0) {
            return s_cmd.registry[i].handler;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * ACK send helper
 * ------------------------------------------------------------------ */

static void send_ack(const gw_message_t *request, bool success, int int_value)
{
    if (s_cmd.notify_fn == NULL) return;

    gw_message_t ack;
    gw_build_ack(&ack, request, s_cmd.has_device_id ? s_cmd.device_id : NULL,
                 success, int_value);

    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_message_encode(&ack, buf, sizeof(buf));
    if (encoded <= 0) {
        ESP_LOGE(TAG, "ACK encode failed: %d", encoded);
        return;
    }

    int rc = s_cmd.notify_fn(buf, (size_t)encoded);
    if (rc != 0) {
        ESP_LOGW(TAG, "ACK notify failed: %d", rc);
    }
}

/* ------------------------------------------------------------------ *
 * Command worker task (docs §24, §25)
 * ------------------------------------------------------------------ */

static void cmd_worker(void *arg)
{
    cmd_rx_msg_t rx;

    while (1) {
        if (xQueueReceive(s_cmd.rx_queue, &rx, portMAX_DELAY) != pdTRUE)
            continue;

        /* Decode CBOR (strict, per gateway_protocol contract). */
        gw_message_t msg;
        int rc = gw_message_decode(rx.data, rx.len, &msg);
        if (rc != GW_OK) {
            ESP_LOGW(TAG, "CBOR decode failed: %s", gw_result_str(rc));
            continue;
        }

        /* Validate message type — only device_command accepted (doc §5). */
        if (strcmp(msg.type, GW_MSG_TYPE_DEVICE_COMMAND) != 0) {
            ESP_LOGW(TAG, "unexpected type: %s", msg.type);
            continue;
        }

        /* Lookup handler. */
        device_cmd_handler_t handler = find_handler(msg.command);
        if (handler == NULL) {
            ESP_LOGW(TAG, "unknown command: %s", msg.command);
            send_ack(&msg, false, 0);
            continue;
        }

        /* Execute handler. */
        device_cmd_response_t response = { 0 };
        device_cmd_result_t result = handler(&msg, &response);

        if (result == DEVICE_CMD_OK && response.long_running) {
            ESP_LOGI(TAG, "cmd accepted (long-running): %s", msg.command);
            send_ack(&msg, true, 0);
        } else if (result == DEVICE_CMD_OK) {
            send_ack(&msg, response.success, response.int_value);
        } else {
            ESP_LOGW(TAG, "handler error: %s -> %d", msg.command, (int)result);
            send_ack(&msg, false, 0);
        }
    }
}

/* ------------------------------------------------------------------ *
 * Built-in commands (doc §27)
 * ------------------------------------------------------------------ */

static device_cmd_result_t cmd_ping_handler(
    const gw_message_t *request, device_cmd_response_t *response)
{
    response->success = true;
    return DEVICE_CMD_OK;
}

static device_cmd_result_t cmd_get_info_handler(
    const gw_message_t *request, device_cmd_response_t *response)
{
    response->success = true;
    response->int_value = GW_PROTOCOL_VERSION;
    return DEVICE_CMD_OK;
}

static device_cmd_result_t cmd_get_state_handler(
    const gw_message_t *request, device_cmd_response_t *response)
{
    response->success = true;
    response->int_value = 0;
    return DEVICE_CMD_OK;
}

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

int device_command_init(int (*notify_fn)(const uint8_t *, size_t))
{
    memset(&s_cmd, 0, sizeof(s_cmd));
    s_cmd.notify_fn = notify_fn;

    s_cmd.rx_queue = xQueueCreate(CMD_RX_QUEUE_DEPTH, sizeof(cmd_rx_msg_t));
    if (s_cmd.rx_queue == NULL) return -1;

    /* Register common commands (doc §27). */
    device_command_register("ping", cmd_ping_handler);
    device_command_register("get_info", cmd_get_info_handler);
    device_command_register("get_state", cmd_get_state_handler);

    ESP_LOGI(TAG, "initialized (%d built-in commands)", s_cmd.registry_count);
    return 0;
}

int device_command_register(const char *command, device_cmd_handler_t handler)
{
    if (command == NULL || handler == NULL) return -1;
    if (s_cmd.frozen) return -1;
    if (s_cmd.registry_count >= CMD_REGISTRY_MAX) return -1;
    if (find_handler(command) != NULL) return -1;

    cmd_entry_t *entry = &s_cmd.registry[s_cmd.registry_count];
    strlcpy(entry->command, command, sizeof(entry->command));
    entry->handler = handler;
    s_cmd.registry_count++;
    return 0;
}

int device_command_freeze(void)
{
    if (s_cmd.frozen) return 0;
    s_cmd.frozen = true;

    xTaskCreate(cmd_worker, "cmd_worker", CMD_TASK_STACK,
                NULL, CMD_TASK_PRIORITY, &s_cmd.worker_task);

    ESP_LOGI(TAG, "frozen (%d commands registered)", s_cmd.registry_count);
    return 0;
}

int device_command_submit(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > GW_MSG_MAX_LEN) return -1;
    if (s_cmd.rx_queue == NULL) return -1;

    cmd_rx_msg_t rx = { .len = (uint16_t)len };
    memcpy(rx.data, data, len);

    if (xQueueSend(s_cmd.rx_queue, &rx, 0) != pdTRUE) {
        ESP_LOGW(TAG, "CMD RX queue full, dropping");
        return -1;
    }
    return 0;
}

int device_command_complete(const gw_message_t *request,
                            const device_cmd_response_t *response)
{
    if (request == NULL || response == NULL) return -1;
    send_ack(request, response->success, response->int_value);
    return 0;
}

void device_command_set_device_id(const char *id)
{
    if (id == NULL) return;
    strlcpy(s_cmd.device_id, id, sizeof(s_cmd.device_id));
    s_cmd.has_device_id = true;
}
