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
 *   bool_value = command success/failure
 *   int_value = legacy/general result value
 *
 *   Structured feature state (when command mutates a semantic feature):
 *     feature_id, property_id, feature_value_bool / feature_value_int
 *
 *   Writable semantic feature handlers SHOULD include the actual
 *   post-command feature state in the ACK.
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
#include "gateway_settings.h"
#include "ble_peripheral.h"
#include "device_feature.h"
#include "device_settings.h"

static const char *TAG = "device_command";

/* ------------------------------------------------------------------ *
 * Shared RX message type (same layout as ble_peripheral rx_msg_t)
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t type;
    uint16_t len;
    uint32_t generation;
    uint8_t data[GW_MSG_MAX_LEN];
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
    uint32_t settings_generation;
    int (*restart_fn)(uint32_t delay_ms);
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
/* Settings v2 command handling (Phase 0). */
static int handle_settings_command(const gw_message_t *msg);

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
    if (device_settings_is_supported()) {
        message->settings_supported = 1;
        message->has_settings_supported = 1;
        message->settings_schema_revision = 1;
        message->has_settings_schema_revision = 1;
    }
}

static int send_capability_message(uint8_t storage[GW_MSG_MAX_LEN],
                                   const gw_message_t *message)
{
    int encoded = gw_message_encode(message, storage, GW_MSG_MAX_LEN);
    if (encoded <= 0) return encoded;
    return ble_peripheral_notify_sequence_send(
        storage, (size_t)encoded, pdMS_TO_TICKS(1000));
}

static int validate_feature_bindings(void)
{
    for (size_t i = 0; i < device_feature_count(); i++) {
        const device_feature_descriptor_t *feature = device_feature_get(i);
        if (feature == NULL || !feature->property.writable) continue;

        cmd_entry_t *entry = find_entry(feature->write_tool);
        if (entry == NULL || !entry->advertised ||
            (uint8_t)entry->value_type !=
                (uint8_t)feature->property.value_type) {
            ESP_LOGE(TAG, "feature '%s' has invalid write tool binding '%s'",
                     feature->feature_id, feature->write_tool);
            return -1;
        }
        if (strcmp(entry->unit, feature->unit) != 0) {
            ESP_LOGE(TAG, "feature '%s' unit '%s' mismatches tool '%s' unit '%s'",
                     feature->feature_id, feature->unit, entry->command,
                     entry->unit);
            return -1;
        }
        if (feature->property.value_type == DEVICE_FEATURE_VALUE_INT &&
            (entry->min_value > entry->max_value || entry->step == 0u)) {
            ESP_LOGE(TAG, "feature '%s' has invalid numeric tool range",
                     feature->feature_id);
            return -1;
        }

        int bindings = 0;
        for (size_t j = 0; j < device_feature_count(); j++) {
            const device_feature_descriptor_t *other = device_feature_get(j);
            if (other != NULL && other->property.writable &&
                strcmp(other->write_tool, feature->write_tool) == 0) {
                bindings++;
            }
        }
        if (bindings > 1) {
            ESP_LOGE(TAG, "write tool '%s' is bound to multiple features",
                     feature->write_tool);
            return -1;
        }
    }
    return 0;
}

/* Emits begin -> item[0..N-1] -> end -> ACK as one ordered stream. */
static int send_capabilities(const gw_message_t *request)
{
    if (request->protocol_version != GW_PROTOCOL_VERSION ||
        !request->has_device_id || !request->has_request_id) {
        return -1;
    }

    int tool_total = advertised_count();
    int feature_total = (int)device_feature_count();
    if (tool_total > DEVICE_COMMAND_MAX_CAPABILITIES ||
        feature_total > DEVICE_FEATURE_MAX_PER_DEVICE) return -1;
    uint8_t storage[GW_MSG_MAX_LEN];
    if (ble_peripheral_notify_sequence_begin() != 0) return -1;

    uint32_t snapshot_id = ++s_cmd.next_snapshot_id;
    if (snapshot_id == 0u) snapshot_id = ++s_cmd.next_snapshot_id;
    gw_message_t message;
    init_capability_message(&message, request,
                            GW_MSG_TYPE_CAPABILITIES_BEGIN, snapshot_id);
    message.total = (uint16_t)tool_total;
    message.has_total = 1;
    message.feature_total = (uint16_t)feature_total;
    message.has_feature_total = 1;
    message.capability_revision = s_cmd.capability_revision;
    message.has_capability_revision = 1;
    if (send_capability_message(storage, &message) != 0) {
        goto fail;
    }

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
        if (send_capability_message(storage, &message) != 0) {
            goto fail;
        }
    }

    for (size_t i = 0; i < device_feature_count(); i++) {
        const device_feature_descriptor_t *feature = device_feature_get(i);
        init_capability_message(&message, request,
                                GW_MSG_TYPE_FEATURE_ITEM, snapshot_id);
        message.sequence = sequence++;
        message.has_sequence = 1;
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
        strlcpy(message.capability_label, feature->title,
                sizeof(message.capability_label));
        strlcpy(message.capability_unit, feature->unit,
                sizeof(message.capability_unit));
        message.feature_decimals = feature->decimals;
        message.has_feature_decimals = 1;
        if (feature->write_tool[0] != '\0') {
            strlcpy(message.feature_tool, feature->write_tool,
                    sizeof(message.feature_tool));
            message.has_feature_tool = 1;
        }
        if (send_capability_message(storage, &message) != 0) {
            goto fail;
        }
    }

    init_capability_message(&message, request,
                            GW_MSG_TYPE_CAPABILITIES_END, snapshot_id);
    message.total = (uint16_t)tool_total;
    message.has_total = 1;
    message.feature_total = (uint16_t)feature_total;
    message.has_feature_total = 1;
    message.bool_value = 1;
    if (send_capability_message(storage, &message) != 0) {
        goto fail;
    }

    gw_build_ack(&message, request, request->device_id, true, 0);
    if (send_capability_message(storage, &message) != 0) {
        goto fail;
    }

    ble_peripheral_notify_sequence_end();
    return 0;

fail:
    ble_peripheral_notify_sequence_abort();
    ble_peripheral_notify_sequence_end();
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
    if (response->has_feature_state) {
        strlcpy(ack.feature_id, response->feature_state.feature_id,
                sizeof(ack.feature_id));
        ack.has_feature_id = 1;
        ack.property_id = response->feature_state.property_id;
        ack.has_property_id = 1;
        if (response->feature_state.value.type == DEVICE_FEATURE_VALUE_BOOL) {
            ack.feature_value_bool = response->feature_state.value.value.bool_value;
            ack.has_feature_value_bool = 1;
        } else if (response->feature_state.value.type ==
                   DEVICE_FEATURE_VALUE_INT) {
            ack.feature_value_int = response->feature_state.value.value.int_value;
            ack.has_feature_value_int = 1;
        }
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

        if (rx.type == DEVICE_CMD_EVENT_SETTINGS_DISCONNECT) {
            device_settings_tx_on_disconnect();
            continue;
        }
        if (rx.type == DEVICE_CMD_EVENT_SETTINGS_TIMEOUT) {
            if (rx.generation == s_cmd.settings_generation) {
                device_settings_confirm_timeout_cb(NULL);
            }
            continue;
        }

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
            device_feature_value_t value;
            int read_rc = (!msg.has_feature_id || !msg.has_property_id) ? -1 :
                device_feature_read(msg.feature_id, msg.property_id, &value);
            response.success = (read_rc == 0);
            response.int_value = (read_rc == 0 &&
                                  value.type == DEVICE_FEATURE_VALUE_BOOL) ?
                                 (value.value.bool_value ? 1 : 0) :
                                 (read_rc == 0 ? value.value.int_value : 0);
            if (read_rc == 0) {
                response.has_feature_state = true;
                strlcpy(response.feature_state.feature_id, msg.feature_id,
                        sizeof(response.feature_state.feature_id));
                response.feature_state.property_id = msg.property_id;
                response.feature_state.value = value;
            }
            send_ack(&msg, &response);
            continue;
        }

        /* Settings v2 commands (Phase 0). */
        if (strcmp(msg.command, GW_COMMAND_DESCRIBE_SETTINGS) == 0 ||
            strcmp(msg.command, GW_COMMAND_READ_SETTINGS) == 0 ||
            strcmp(msg.command, GW_MSG_TYPE_SETTINGS_TX_BEGIN) == 0 ||
            strcmp(msg.command, GW_MSG_TYPE_SETTINGS_TX_SET) == 0 ||
            strcmp(msg.command, GW_MSG_TYPE_SETTINGS_TX_COMMIT) == 0 ||
            strcmp(msg.command, GW_MSG_TYPE_SETTINGS_TX_ABORT) == 0) {
            handle_settings_command(&msg);
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
 * Settings v2 command handling (Phase 2 — BLE Discovery & Streaming).
 *
 * Handles describe_settings, read_settings, and transaction commands.
 * Streams schema/value frames via bounded notify sequence.
 * ------------------------------------------------------------------ */

/* Schema revision — increment when descriptor schema changes. */
#define DEVICE_SETTINGS_SCHEMA_REVISION 1

/* Settings-specific error codes (for ACK int_value on failure). */
enum {
    SETTINGS_ERR_OK                = 0,
    SETTINGS_ERR_INVALID_ARGUMENT  = 1,
    SETTINGS_ERR_UNSUPPORTED       = 2,
    SETTINGS_ERR_TYPE_MISMATCH     = 3,
    SETTINGS_ERR_OUT_OF_RANGE      = 4,
    SETTINGS_ERR_READONLY          = 5,
    SETTINGS_ERR_REVISION_CONFLICT = 6,
    SETTINGS_ERR_TX_BUSY           = 7,
    SETTINGS_ERR_TX_NOT_ACTIVE     = 8,
    SETTINGS_ERR_TX_ID_MISMATCH    = 9,
    SETTINGS_ERR_VALIDATION_FAILED = 10,
    SETTINGS_ERR_PERSIST_FAILED    = 11,
    SETTINGS_ERR_INTERNAL_ERROR    = 12,
};

/* Map esp_err_t to settings-specific error code. */
static int settings_err_to_code(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                return SETTINGS_ERR_OK;
    case ESP_ERR_INVALID_ARG:   return SETTINGS_ERR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE: return SETTINGS_ERR_TX_NOT_ACTIVE;
    case ESP_ERR_NOT_FOUND:     return SETTINGS_ERR_INTERNAL_ERROR;
    default:                    return SETTINGS_ERR_INTERNAL_ERROR;
    }
}

/* Encode + send one frame in a notify sequence. */
static uint8_t settings_type_to_wire(device_setting_type_t type)
{
    switch (type) {
    case DEVICE_SETTING_BOOL: return GW_SETTING_TYPE_BOOL;
    case DEVICE_SETTING_INT: return GW_SETTING_TYPE_INT;
    case DEVICE_SETTING_STRING: return GW_SETTING_TYPE_STRING;
    case DEVICE_SETTING_ENUM: return GW_SETTING_TYPE_ENUM;
    default: return GW_SETTING_TYPE_NONE;
    }
}

static bool settings_wire_type_to_device(uint8_t wire_type,
                                         device_setting_type_t *out_type)
{
    if (out_type == NULL) return false;
    switch (wire_type) {
    case GW_SETTING_TYPE_BOOL: *out_type = DEVICE_SETTING_BOOL; return true;
    case GW_SETTING_TYPE_INT: *out_type = DEVICE_SETTING_INT; return true;
    case GW_SETTING_TYPE_STRING: *out_type = DEVICE_SETTING_STRING; return true;
    case GW_SETTING_TYPE_ENUM: *out_type = DEVICE_SETTING_ENUM; return true;
    case GW_SETTING_TYPE_BOOL_LEGACY: *out_type = DEVICE_SETTING_BOOL; return true;
    default: return false;
    }
}

static int send_settings_frame(uint8_t storage[GW_MSG_MAX_LEN], int encoded)
{
    if (encoded <= 0) return encoded;
    return ble_peripheral_notify_sequence_send(
        storage, (size_t)encoded, pdMS_TO_TICKS(1000));
}

/* Send ACK as last frame in a notify sequence. */
static int send_settings_ack(uint8_t storage[GW_MSG_MAX_LEN],
                             const gw_message_t *msg, bool success)
{
    gw_message_t ack;
    gw_build_ack(&ack, msg, msg->device_id, success, 0);
    int encoded = gw_message_encode(&ack, storage, GW_MSG_MAX_LEN);
    return send_settings_frame(storage, encoded);
}

/* Stream describe_settings response:
 *   settings_begin -> setting_item* -> setting_option_item* -> settings_end -> ACK */
static int handle_describe_settings(const gw_message_t *msg)
{
    uint16_t total = (uint16_t)device_settings_count();
    uint32_t request_id = msg->has_request_id ? msg->request_id : 0;

    uint8_t storage[GW_MSG_MAX_LEN];
    if (ble_peripheral_notify_sequence_begin() != 0) {
        ESP_LOGW(TAG, "describe_settings: notify sequence begin failed");
        return -1;
    }

    /* settings_begin */
    int enc = gw_settings_encode_begin(storage, sizeof(storage),
                                       total, request_id);
    if (send_settings_frame(storage, enc) != 0) goto fail;

    /* setting_item per descriptor */
    for (uint16_t i = 0; i < total; i++) {
        const device_setting_descriptor_t *desc = device_settings_get(i);
        if (desc == NULL) goto fail;

        enc = gw_settings_encode_item(storage, sizeof(storage),
                                      i, total, request_id,
                                      desc->id, desc->title,
                                      desc->group ? desc->group : "",
                                      desc->unit ? desc->unit : "",
                                      (uint8_t)desc->type, desc->flags,
                                      desc->max_length,
                                      desc->min_value, desc->max_value,
                                      (uint32_t)desc->step);
        if (send_settings_frame(storage, enc) != 0) goto fail;

        /* Emit enum options after the setting_item */
        if (desc->type == DEVICE_SETTING_ENUM &&
            desc->options != NULL && desc->option_count > 0) {
            for (uint8_t j = 0; j < desc->option_count; j++) {
                enc = gw_settings_encode_option_item(storage, sizeof(storage),
                                                     i, j, request_id,
                                                     desc->options[j].label);
                if (send_settings_frame(storage, enc) != 0) goto fail;
            }
        }
    }

    /* settings_end */
    enc = gw_settings_encode_end(storage, sizeof(storage),
                                 total, request_id);
    if (send_settings_frame(storage, enc) != 0) goto fail;

    /* ACK */
    if (send_settings_ack(storage, msg, true) != 0) goto fail;

    ble_peripheral_notify_sequence_end();
    ESP_LOGI(TAG, "describe_settings: streamed %u settings", total);
    return 0;

fail:
    ble_peripheral_notify_sequence_abort();
    ble_peripheral_notify_sequence_end();
    return -1;
}

/* Stream read_settings response:
 *   settings_values_begin -> setting_value* -> settings_values_end -> ACK */
static int handle_read_settings(const gw_message_t *msg)
{
    uint16_t total = (uint16_t)device_settings_count();
    uint32_t revision = device_settings_get_revision();
    uint32_t request_id = msg->has_request_id ? msg->request_id : 0;

    uint8_t storage[GW_MSG_MAX_LEN];
    if (ble_peripheral_notify_sequence_begin() != 0) {
        ESP_LOGW(TAG, "read_settings: notify sequence begin failed");
        return -1;
    }

    /* settings_values_begin */
    int enc = gw_settings_encode_values_begin(storage, sizeof(storage),
                                              total, revision, request_id);
    if (send_settings_frame(storage, enc) != 0) goto fail;

    /* setting_value per descriptor */
    for (uint16_t i = 0; i < total; i++) {
        const device_setting_descriptor_t *desc = device_settings_get(i);
        void *read_ctx = desc != NULL ?
                         (desc->read_ctx != NULL ? desc->read_ctx : desc->ctx) : NULL;
        if (desc == NULL || read_ctx == NULL ||
            (desc->read == NULL && desc->read_cb.read_bool == NULL)) {
            goto fail;
        }

        /* Secret settings emit only configured/not-configured */
        if (desc->flags & DEVICE_SETTING_FLAG_SECRET) {
            device_setting_secret_value_t secret;
            memset(&secret, 0, sizeof(secret));
            esp_err_t err = desc->read != NULL ?
                desc->read(read_ctx, &secret) : ESP_ERR_INVALID_ARG;
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "read_settings: secret read failed for '%s': %d",
                         desc->id, (int)err);
                goto fail;
            }
            bool configured = secret.configured;
            enc = gw_settings_encode_value(storage, sizeof(storage),
                                           i, desc->id, settings_type_to_wire(desc->type),
                                           &configured, request_id);
        } else {
            /* Read value into stack buffer */
            uint8_t value_buf[64];
            memset(value_buf, 0, sizeof(value_buf));
            esp_err_t err = desc->read != NULL ?
                desc->read(read_ctx, value_buf) : ESP_ERR_INVALID_ARG;
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "read_settings: read failed for '%s': %d",
                         desc->id, (int)err);
                goto fail;
            }
            enc = gw_settings_encode_value(storage, sizeof(storage),
                                           i, desc->id, (uint8_t)desc->type,
                                           value_buf, request_id);
        }
        if (send_settings_frame(storage, enc) != 0) goto fail;
    }

    /* settings_values_end */
    enc = gw_settings_encode_values_end(storage, sizeof(storage),
                                        total, revision, request_id);
    if (send_settings_frame(storage, enc) != 0) goto fail;

    /* ACK */
    if (send_settings_ack(storage, msg, true) != 0) goto fail;

    ble_peripheral_notify_sequence_end();
    ESP_LOGI(TAG, "read_settings: streamed %u values (rev=%lu)",
             total, (unsigned long)revision);
    return 0;

fail:
    ble_peripheral_notify_sequence_abort();
    ble_peripheral_notify_sequence_end();
    return -1;
}

/* Handle transaction commands (TX_BEGIN, TX_SET, TX_COMMIT, TX_ABORT).
 *
 * Rules (Phase 3):
 *   - Only one transaction active at a time (serialized).
 *   - Transaction ID checked on every tx command.
 *   - BEGIN: idempotent if same tx_id + same expected_revision.
 *   - SET: bounded value copy, no heap allocation for scalars.
 *   - COMMIT: validates all staged changes, NVS atomic commit,
 *             state -> COMMITTED_WAIT_CONFIRM. No restart here.
 *   - ABORT: safe to call even if no transaction active (idempotent).
 *   - ACK int_value: current_revision (BEGIN), new_revision (COMMIT),
 *                    error code on failure.
 */
static int handle_settings_tx_command(const gw_message_t *msg)
{
    gw_settings_command_t cmd;
    if (gw_settings_decode_command(msg, &cmd) != GW_OK) {
        ESP_LOGW(TAG, "settings_tx: decode failed");
        device_cmd_response_t resp = { .success = false,
                                       .int_value = SETTINGS_ERR_INVALID_ARGUMENT };
        send_ack(msg, &resp);
        return -1;
    }

    esp_err_t err = ESP_OK;
    device_cmd_response_t response = { 0 };
    bool arm_confirm_timeout = false;
    bool schedule_restart = false;
    uint64_t confirm_tx_id = 0;
    uint32_t confirm_revision = 0;

    switch (cmd.cmd_type) {
    case GW_SETTINGS_CMD_TX_BEGIN: {
        if (!cmd.has_transaction_id || !cmd.has_expected_revision) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = device_settings_tx_begin(cmd.transaction_id,
                                       cmd.expected_revision);
        if (err == ESP_OK) {
            /* ACK carries current_revision (before transaction). */
            response.int_value = (int)device_settings_get_revision();
        }
        break;
    }
    case GW_SETTINGS_CMD_TX_SET: {
        if (!cmd.has_transaction_id || !cmd.has_setting_id ||
            !cmd.has_setting_value) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        const void *value_ptr = NULL;
        device_setting_type_t device_type;
        if (!settings_wire_type_to_device(cmd.setting_value.type, &device_type)) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        switch (device_type) {
        case DEVICE_SETTING_BOOL:
            value_ptr = &cmd.setting_value.value.bool_val;
            break;
        case DEVICE_SETTING_INT:
            value_ptr = &cmd.setting_value.value.int_val;
            break;
        case DEVICE_SETTING_STRING:
            /* String is already bounded copy in gw_settings_command_t.
             * The stage callback will copy into the staging config. */
            value_ptr = cmd.setting_value.value.str_val;
            break;
        case DEVICE_SETTING_ENUM:
            value_ptr = &cmd.setting_value.value.enum_val;
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        if (err == ESP_OK) {
            err = device_settings_tx_set_with_id(
                cmd.transaction_id, cmd.setting_id,
                device_type, value_ptr);
        }
        break;
    }
    case GW_SETTINGS_CMD_TX_COMMIT: {
        if (!cmd.has_transaction_id) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        uint32_t new_revision = 0;
        err = device_settings_tx_commit(cmd.transaction_id, &new_revision);
        if (err == ESP_OK) {
            /* ACK carries new_revision after commit. */
            response.int_value = (int)new_revision;
            arm_confirm_timeout = true;
        }
        break;
    }
    case GW_SETTINGS_CMD_TX_ABORT: {
        if (!cmd.has_transaction_id) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        /* ABORT is idempotent — safe to call even if no tx active. */
        err = device_settings_tx_abort(cmd.transaction_id);
        break;
    }
    case GW_SETTINGS_CMD_TX_CONFIRM: {
        if (!cmd.has_transaction_id || !cmd.has_new_revision) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = device_settings_tx_validate_confirm(cmd.transaction_id,
                                                  cmd.new_revision);
        confirm_tx_id = cmd.transaction_id;
        confirm_revision = cmd.new_revision;
        schedule_restart = (err == ESP_OK);
        break;
    }
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    response.success = (err == ESP_OK);
    if (err != ESP_OK) {
        response.int_value = settings_err_to_code(err);
        ESP_LOGW(TAG, "settings_tx: cmd=%d err=%d code=%d",
                 (int)cmd.cmd_type, (int)err, response.int_value);
    } else {
        ESP_LOGI(TAG, "settings_tx: cmd=%d ok (int=%d)",
                 (int)cmd.cmd_type, response.int_value);
    }

    send_ack(msg, &response);
    if (err == ESP_OK && arm_confirm_timeout) {
        device_settings_tx_arm_confirm_timeout();
    }
    if (err == ESP_OK && schedule_restart) {
        err = device_settings_tx_confirm(confirm_tx_id, confirm_revision);
        if (err == ESP_OK && s_cmd.restart_fn != NULL) {
            (void)s_cmd.restart_fn(500);
        }
    }
    return (err == ESP_OK) ? 0 : -1;
}

/* Main settings command dispatcher. */
static int handle_settings_command(const gw_message_t *msg)
{
    if (msg == NULL) return -1;

    if (strcmp(msg->command, GW_COMMAND_DESCRIBE_SETTINGS) == 0) {
        return handle_describe_settings(msg);
    }
    if (strcmp(msg->command, GW_COMMAND_READ_SETTINGS) == 0) {
        return handle_read_settings(msg);
    }
    /* Transaction commands */
    if (strcmp(msg->command, GW_MSG_TYPE_SETTINGS_TX_BEGIN) == 0 ||
        strcmp(msg->command, GW_MSG_TYPE_SETTINGS_TX_SET) == 0 ||
        strcmp(msg->command, GW_MSG_TYPE_SETTINGS_TX_COMMIT) == 0 ||
        strcmp(msg->command, GW_MSG_TYPE_SETTINGS_TX_ABORT) == 0 ||
        strcmp(msg->command, GW_MSG_TYPE_SETTINGS_COMMIT_CONFIRM) == 0) {
        return handle_settings_tx_command(msg);
    }

    ESP_LOGW(TAG, "unknown settings command: %s", msg->command);
    send_ack(msg, &(device_cmd_response_t){ .success = false });
    return -1;
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
    if (validate_feature_bindings() != 0) return -1;
    s_cmd.frozen = true;

    xTaskCreate(cmd_worker, "cmd_worker", CMD_TASK_STACK,
                NULL, CMD_TASK_PRIORITY, &s_cmd.worker_task);

    /* Log capability registry state for debugging (spec D8). */
    ESP_LOGI(TAG, "frozen (%d commands registered, %d advertised)",
             s_cmd.registry_count, advertised_count());
    return 0;
}

int device_command_post_internal_event(device_cmd_event_type_t type,
                                        uint32_t generation)
{
    if (s_cmd.rx_queue == NULL || type == DEVICE_CMD_EVENT_RX_FRAME) return -1;
    cmd_rx_msg_t event = { .type = (uint8_t)type, .generation = generation };
    return xQueueSend(s_cmd.rx_queue, &event, 0) == pdTRUE ? 0 : -1;
}

void device_command_set_restart_fn(int (*fn)(uint32_t delay_ms))
{
    s_cmd.restart_fn = fn;
}

int device_command_submit(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > GW_MSG_MAX_LEN) return -1;
    if (s_cmd.rx_queue == NULL) return -1;

    cmd_rx_msg_t rx = { .type = DEVICE_CMD_EVENT_RX_FRAME,
                        .len = (uint16_t)len };
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

int device_command_response_set_feature_bool(
    device_cmd_response_t *response,
    const char *feature_id,
    uint8_t property_id,
    bool value)
{
    if (response == NULL ||
        feature_id == NULL ||
        feature_id[0] == '\0' ||
        strnlen(feature_id, GW_FEATURE_ID_LEN) >= GW_FEATURE_ID_LEN) {
        return -1;
    }

    response->has_feature_state = true;
    strlcpy(response->feature_state.feature_id, feature_id,
            sizeof(response->feature_state.feature_id));
    response->feature_state.property_id = property_id;
    response->feature_state.value.type = DEVICE_FEATURE_VALUE_BOOL;
    response->feature_state.value.value.bool_value = value;

    return 0;
}

int device_command_response_set_feature_int(
    device_cmd_response_t *response,
    const char *feature_id,
    uint8_t property_id,
    int32_t value)
{
    if (response == NULL || feature_id == NULL || feature_id[0] == '\0' ||
        strnlen(feature_id, GW_FEATURE_ID_LEN) >= GW_FEATURE_ID_LEN) {
        return -1;
    }

    response->has_feature_state = true;
    strlcpy(response->feature_state.feature_id, feature_id,
            sizeof(response->feature_state.feature_id));
    response->feature_state.property_id = property_id;
    response->feature_state.value.type = DEVICE_FEATURE_VALUE_INT;
    response->feature_state.value.value.int_value = value;

    return 0;
}
