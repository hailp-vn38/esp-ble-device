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
#include "device_command.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "gateway_protocol.h"
#include "ble_peripheral.h"
#include "device_feature.h"

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
    bool advertised;
    device_cmd_value_type_t value_type;
    uint8_t flags;
    int32_t min_value;
    int32_t max_value;
    uint32_t step;
    char label[GW_MSG_CAP_LABEL_LEN];
    char unit[GW_MSG_CAP_UNIT_LEN];
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
    uint32_t capability_revision;
    uint32_t next_snapshot_id;
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

static cmd_entry_t *find_entry(const char *command)
{
    for (int i = 0; i < s_cmd.registry_count; i++) {
        if (strcmp(s_cmd.registry[i].command, command) == 0) {
            return &s_cmd.registry[i];
        }
    }
    return NULL;
}

static bool valid_command_name(const char *command)
{
    if (command == NULL) return false;
    size_t length = strnlen(command, GW_MSG_COMMAND_LEN);
    if (length == 0 || length >= GW_MSG_COMMAND_LEN) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)command[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

static int advertised_count(void)
{
    int count = 0;
    for (int i = 0; i < s_cmd.registry_count; i++) {
        if (s_cmd.registry[i].advertised) count++;
    }
    return count;
}

static void init_capability_message(gw_message_t *message,
                                    const gw_message_t *request,
                                    const char *type, uint32_t snapshot_id)
{
    gw_message_init(message);
    strlcpy(message->type, type, sizeof(message->type));
    strlcpy(message->device_id, request->device_id,
            sizeof(message->device_id));
    message->has_device_id = 1;
    strlcpy(message->command, GW_COMMAND_DESCRIBE_CAPABILITIES,
            sizeof(message->command));
    message->snapshot_id = snapshot_id;
    message->has_snapshot_id = 1;
}

static int encode_batch_message(ble_peripheral_notify_item_t *item,
                                uint8_t storage[GW_MSG_MAX_LEN],
                                const gw_message_t *message)
{
    int encoded = gw_message_encode(message, storage, GW_MSG_MAX_LEN);
    if (encoded <= 0) return encoded;
    item->data = storage;
    item->len = (size_t)encoded;
    return GW_OK;
}

/* Emits begin -> item[0..N-1] -> end -> ACK as one transport batch. */
static int send_capabilities(const gw_message_t *request)
{
    if (request->protocol_version < 3u || !request->has_device_id ||
        !request->has_request_id) {
        return -1;
    }

    int tool_total = advertised_count();
    int feature_total = (int)device_feature_count();
    if (tool_total > DEVICE_COMMAND_MAX_CAPABILITIES ||
        feature_total > DEVICE_FEATURE_MAX_PER_DEVICE) return -1;
    size_t batch_count = (size_t)tool_total + (size_t)feature_total + 3u;
    ble_peripheral_notify_item_t *items =
        calloc(batch_count, sizeof(*items));
    uint8_t (*storage)[GW_MSG_MAX_LEN] =
        calloc(batch_count, sizeof(*storage));
    if (items == NULL || storage == NULL) {
        free(items);
        free(storage);
        return -1;
    }

    uint32_t snapshot_id = ++s_cmd.next_snapshot_id;
    if (snapshot_id == 0u) snapshot_id = ++s_cmd.next_snapshot_id;
    size_t out = 0;
    gw_message_t message;
    init_capability_message(&message, request,
                            GW_MSG_TYPE_CAPABILITIES_BEGIN, snapshot_id);
    message.total = (uint16_t)tool_total;
    message.has_total = 1;
    message.feature_total = (uint16_t)feature_total;
    message.has_feature_total = 1;
    message.capability_revision = s_cmd.capability_revision;
    message.has_capability_revision = 1;
    if (encode_batch_message(&items[out], storage[out], &message) != GW_OK) {
        goto fail;
    }
    out++;

    uint16_t sequence = 0;
    for (int i = 0; i < s_cmd.registry_count; i++) {
        const cmd_entry_t *entry = &s_cmd.registry[i];
        if (!entry->advertised) continue;
        init_capability_message(&message, request,
                                GW_MSG_TYPE_CAPABILITY_ITEM, snapshot_id);
        strlcpy(message.command, entry->command, sizeof(message.command));
        message.sequence = sequence++;
        message.has_sequence = 1;
        message.value_type = (uint8_t)entry->value_type;
        message.has_value_type = 1;
        message.capability_flags = entry->flags;
        message.has_capability_flags = 1;
        strlcpy(message.capability_label, entry->label,
                sizeof(message.capability_label));
        strlcpy(message.capability_unit, entry->unit,
                sizeof(message.capability_unit));
        if (entry->value_type == DEVICE_CMD_VALUE_INT) {
            message.min_value = entry->min_value;
            message.has_min_value = 1;
            message.max_value = entry->max_value;
            message.has_max_value = 1;
            message.step = entry->step;
            message.has_step = 1;
        }
        if (encode_batch_message(&items[out], storage[out], &message) != GW_OK) {
            goto fail;
        }
        out++;
    }

    for (size_t i = 0; i < device_feature_count(); i++) {
        const device_feature_descriptor_t *feature = device_feature_get(i);
        init_capability_message(&message, request,
                                GW_MSG_TYPE_FEATURE_ITEM, snapshot_id);
        strlcpy(message.feature_id, feature->feature_id,
                sizeof(message.feature_id));
        message.has_feature_id = 1;
        message.feature_type = (uint8_t)feature->type;
        message.has_feature_type = 1;
        message.feature_schema_version = feature->schema_version;
        message.has_feature_schema_version = 1;
        message.feature_flags = feature->flags;
        message.has_feature_flags = 1;
        message.property_id = feature->property.id;
        message.has_property_id = 1;
        strlcpy(message.feature_tool, feature->write_tool,
                sizeof(message.feature_tool));
        message.has_feature_tool = 1;
        message.value_type = feature->property.value_type;
        message.has_value_type = 1;
        if (encode_batch_message(&items[out], storage[out], &message) != GW_OK) {
            goto fail;
        }
        out++;
    }

    init_capability_message(&message, request,
                            GW_MSG_TYPE_CAPABILITIES_END, snapshot_id);
    message.total = (uint16_t)tool_total;
    message.has_total = 1;
    message.feature_total = (uint16_t)feature_total;
    message.has_feature_total = 1;
    message.bool_value = 1;
    if (encode_batch_message(&items[out], storage[out], &message) != GW_OK) {
        goto fail;
    }
    out++;

    gw_build_ack(&message, request, request->device_id, true, 0);
    if (encode_batch_message(&items[out], storage[out], &message) != GW_OK) {
        goto fail;
    }
    out++;

    int rc = ble_peripheral_notify_batch(items, out);
    free(storage);
    free(items);
    return rc;

fail:
    free(storage);
    free(items);
    return -1;
}

/* ------------------------------------------------------------------ *
 * ACK send helper
 * ------------------------------------------------------------------ */

static void send_ack(const gw_message_t *request,
                     const device_cmd_response_t *response)
{
    if (s_cmd.notify_fn == NULL) return;

    gw_message_t ack;
    /* Spec D2/D3: ACK must always echo request->device_id (Gateway routing
     * identity). Never override with configured native ID (s_cmd.device_id). */
    gw_build_ack(&ack, request, request->device_id, response->success,
                 response->int_value);
    if (response->has_feature_value_bool) {
        strlcpy(ack.feature_id, response->feature_id,
                sizeof(ack.feature_id));
        ack.has_feature_id = 1;
        ack.property_id = response->feature_property_id;
        ack.has_property_id = 1;
        ack.feature_value_bool = response->feature_value_bool;
        ack.has_feature_value_bool = 1;
    }

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

        if (strcmp(msg.command, GW_COMMAND_DESCRIBE_CAPABILITIES) == 0) {
            if (send_capabilities(&msg) != 0) {
                ESP_LOGW(TAG, "capability response failed");
                device_cmd_response_t response = { .success = false };
                send_ack(&msg, &response);
            }
            continue;
        }

        if (strcmp(msg.command, GW_COMMAND_READ_FEATURE_STATE) == 0) {
            device_cmd_response_t response = { 0 };
            bool value = false;
            int read_rc = device_feature_read_bool(
                msg.feature_id, msg.property_id, &value);
            response.success = (read_rc == 0);
            response.int_value = value ? 1 : 0;
            if (read_rc == 0) {
                response.has_feature_value_bool = true;
                response.feature_value_bool = value;
                response.feature_property_id = msg.property_id;
                strlcpy(response.feature_id, msg.feature_id,
                        sizeof(response.feature_id));
            }
            send_ack(&msg, &response);
            continue;
        }

        /* Lookup handler. */
        device_cmd_handler_t handler = find_handler(msg.command);
        if (handler == NULL) {
            ESP_LOGW(TAG, "unknown command: %s", msg.command);
            device_cmd_response_t response = { .success = false };
            send_ack(&msg, &response);
            continue;
        }

        /* Execute handler. */
        device_cmd_response_t response = { 0 };
        device_cmd_result_t result = handler(&msg, &response);

        if (result == DEVICE_CMD_OK && response.long_running) {
            ESP_LOGI(TAG, "cmd accepted (long-running): %s", msg.command);
            device_cmd_response_t ack = { .success = true };
            send_ack(&msg, &ack);
        } else if (result == DEVICE_CMD_OK) {
            send_ack(&msg, &response);
        } else {
            ESP_LOGW(TAG, "handler error: %s -> %d", msg.command, (int)result);
            device_cmd_response_t ack = { .success = false };
            send_ack(&msg, &ack);
        }
    }
}

/* ------------------------------------------------------------------ *
 * Built-in commands (spec D6, §16)
 *
 * Policy:
 *   - ping: INTERNAL by default, NOT advertised
 *   - get_info: INTERNAL by default, NOT advertised
 *   - get_state: INTERNAL by default, but can be PROMOTED by product
 *                via device_command_register_capability()
 *
 * Reference product promotes get_state to PUBLIC (spec §17.2).
 * ping/get_info remain INTERNAL unless explicitly promoted.
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
    s_cmd.capability_revision = 1;

    s_cmd.rx_queue = xQueueCreate(CMD_RX_QUEUE_DEPTH, sizeof(cmd_rx_msg_t));
    if (s_cmd.rx_queue == NULL) return -1;

    /* Register built-in commands in deterministic order (spec D6, D8).
     * This order is frozen and becomes the PRESENTATION ORDER for
     * capability responses. Built-ins are INTERNAL by default. */
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

int device_command_register_capability(
    const device_cmd_capability_t *capability,
    device_cmd_handler_t handler)
{
    if (capability == NULL || handler == NULL || s_cmd.frozen ||
        !valid_command_name(capability->command) ||
        capability->value_type > DEVICE_CMD_VALUE_INT ||
        (capability->value_type == DEVICE_CMD_VALUE_INT &&
         (capability->min_value > capability->max_value ||
          capability->step == 0u)) ||
        (capability->label != NULL &&
         strnlen(capability->label, GW_MSG_CAP_LABEL_LEN) >=
             GW_MSG_CAP_LABEL_LEN) ||
        (capability->unit != NULL &&
         strnlen(capability->unit, GW_MSG_CAP_UNIT_LEN) >=
             GW_MSG_CAP_UNIT_LEN)) {
        return -1;
    }

    cmd_entry_t *entry = find_entry(capability->command);
    if (entry == NULL) {
        /* New capability — add at end of registry (spec D8).
         * Registry order is DETERMINISTIC and becomes PRESENTATION ORDER.
         * Must not exceed max public capabilities (spec §4.6). */
        if (s_cmd.registry_count >= CMD_REGISTRY_MAX ||
            advertised_count() >= DEVICE_COMMAND_MAX_CAPABILITIES) {
            return -1;
        }
        entry = &s_cmd.registry[s_cmd.registry_count++];
        memset(entry, 0, sizeof(*entry));
        strlcpy(entry->command, capability->command, sizeof(entry->command));
    } else if (!entry->advertised &&
               advertised_count() >= DEVICE_COMMAND_MAX_CAPABILITIES) {
        /* Existing non-advertised command cannot be promoted if at limit. */
        return -1;
    }

    entry->handler = handler;
    entry->advertised = true;
    entry->value_type = capability->value_type;
    entry->flags = capability->flags;
    entry->min_value = capability->min_value;
    entry->max_value = capability->max_value;
    entry->step = capability->step;
    strlcpy(entry->label,
            capability->label != NULL ? capability->label
                                      : capability->command,
            sizeof(entry->label));
    strlcpy(entry->unit, capability->unit != NULL ? capability->unit : "",
            sizeof(entry->unit));
    return 0;
}

int device_command_freeze(void)
{
    if (s_cmd.frozen) return 0;
    s_cmd.frozen = true;

    xTaskCreate(cmd_worker, "cmd_worker", CMD_TASK_STACK,
                NULL, CMD_TASK_PRIORITY, &s_cmd.worker_task);

    /* Log capability registry state for debugging (spec D8). */
    ESP_LOGI(TAG, "frozen (%d commands registered, %d advertised)",
             s_cmd.registry_count, advertised_count());
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
    send_ack(request, response);
    return 0;
}

/* DEPRECATED: Do not use. ACK routing identity is always request->device_id.
 * This function exists for backward compatibility only. */
void device_command_set_device_id(const char *id)
{
    if (id == NULL) return;
    ESP_LOGW(TAG, "device_command_set_device_id() is deprecated; ACK routing uses request->device_id");
    strlcpy(s_cmd.device_id, id, sizeof(s_cmd.device_id));
    s_cmd.has_device_id = true;
}

void device_command_set_capability_revision(uint32_t revision)
{
    /* Spec D7: Capability revision belongs to product/schema owner.
     * Must increment when public capability schema changes:
     * - add/remove public command
     * - change value type
     * - change flags
     * - change min/max/step
     * - change label/unit
     * - change public command presentation order (if intentionally changed)
     *
     * Does NOT need to increment when runtime value changes. */
    s_cmd.capability_revision = revision;
}
