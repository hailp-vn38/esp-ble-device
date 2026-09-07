/*
 * gateway_protocol — ESP-GATT Protocol v4 wire contract (Device side).
 *
 * Single source of truth for protocol constants and CBOR message codec
 * shared with esp-ble-gateway. DO NOT fork per product. When the shared
 * `esp-gatt-protocol` component becomes available, this component is
 * replaced by it without API changes at call sites.
 */
#ifndef GATEWAY_PROTOCOL_H
#define GATEWAY_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Protocol constants (must match Gateway cbor_codec / ble_central)
 * ------------------------------------------------------------------ */

#define GW_PROTOCOL_VERSION 4u

/* BLE contract: Peripheral exposes these; Gateway discovers them. */
#define GW_BLE_SERVICE_UUID 0xABF0u /* Primary service              */
#define GW_BLE_COMMAND_UUID 0xABF1u /* Gateway -> Device, WriteNoRsp*/
#define GW_BLE_STATUS_UUID  0xABF2u /* Device -> Gateway, Notify    */
#define GW_BLE_CCCD_UUID    0x2902u /* CCCD for STATUS              */

/* Message size limits. Effective string max = LEN - 1 (NUL kept). */
#define GW_MSG_MAX_LEN           256u
#define GW_SETTINGS_MIN_MTU      247u
#define GW_SETTINGS_TARGET_ATT_PAYLOAD 244u
#define GW_MSG_TYPE_LEN           24u
#define GW_MSG_DEVICE_ID_LEN      32u
#define GW_MSG_COMMAND_LEN        32u
#define GW_MSG_NAME_LEN           32u
#define GW_MSG_CAP_LABEL_LEN      32u
#define GW_MSG_CAP_UNIT_LEN       12u
#define GW_FEATURE_ID_LEN          32u

/* Known wire message types. Device RX handles only device_command;
 * Device TX emits device_ack / device_event. */
#define GW_MSG_TYPE_GATEWAY_COMMAND "gateway_command"
#define GW_MSG_TYPE_DEVICE_COMMAND  "device_command"
#define GW_MSG_TYPE_DEVICE_ACK      "device_ack"
#define GW_MSG_TYPE_DEVICE_EVENT    "device_event"
#define GW_MSG_TYPE_CAPABILITIES_BEGIN "capabilities_begin"
#define GW_MSG_TYPE_CAPABILITY_ITEM    "capability_item"
#define GW_MSG_TYPE_CAPABILITIES_END   "capabilities_end"
#define GW_MSG_TYPE_FEATURE_ITEM        "feature_item"
#define GW_COMMAND_DESCRIBE_CAPABILITIES "describe_capabilities"
#define GW_COMMAND_READ_FEATURE_STATE    "read_feature_state"
#define GW_EVENT_FEATURE_STATE           "feature_state"

/* Settings v2 message types (Phase 0 — additive, backward-compatible). */
#define GW_MSG_TYPE_SETTINGS_BEGIN       "settings_begin"
#define GW_MSG_TYPE_SETTINGS_ITEM        "settings_item"
#define GW_MSG_TYPE_SETTINGS_OPTION_ITEM "settings_option_item"
#define GW_MSG_TYPE_SETTINGS_END         "settings_end"
#define GW_MSG_TYPE_SETTINGS_VALUES_BEGIN "settings_values_begin"
#define GW_MSG_TYPE_SETTINGS_VALUE        "settings_values_value"
#define GW_MSG_TYPE_SETTINGS_VALUES_END   "settings_values_end"
#define GW_MSG_TYPE_SETTINGS_ITEM_LEGACY        "setting_item"
#define GW_MSG_TYPE_SETTINGS_OPTION_ITEM_LEGACY "setting_option_item"
#define GW_MSG_TYPE_SETTINGS_VALUE_LEGACY       "setting_value"
#define GW_MSG_TYPE_SETTINGS_TX_BEGIN     "settings_tx_begin"
#define GW_MSG_TYPE_SETTINGS_TX_SET       "settings_tx_set"
#define GW_MSG_TYPE_SETTINGS_TX_COMMIT    "settings_tx_commit"
#define GW_MSG_TYPE_SETTINGS_TX_ABORT     "settings_tx_abort"
#define GW_MSG_TYPE_SETTINGS_COMMIT_CONFIRM "settings_commit_confirm"
#define GW_COMMAND_DESCRIBE_SETTINGS      "describe_settings"
#define GW_COMMAND_READ_SETTINGS          "read_settings"
#define GW_COMMAND_GET_SETTINGS           "get_settings"
#define GW_COMMAND_SET_SETTINGS           "set_settings"
#define GW_COMMAND_COMMIT_SETTINGS       "commit_settings"

/* CBOR numeric keys (wire contract, do not renumber). */
enum {
    GW_KEY_PROTOCOL_VERSION = 0,
    GW_KEY_TYPE = 1,
    GW_KEY_DEVICE_ID = 2,
    GW_KEY_COMMAND = 3,
    GW_KEY_INT_VALUE = 4,
    GW_KEY_BOOL_VALUE = 5,
    GW_KEY_NAME = 6,
    /* 7 reserved since v4 (former device_type key). Never emitted; ignored
     * on decode. Do not renumber. */
    GW_KEY_RESERVED_7 = 7,
    GW_KEY_BLE_ADDR = 8,
    GW_KEY_BLE_ADDR_TYPE = 9,
    GW_KEY_REQUEST_ID = 10,
    GW_KEY_SNAPSHOT_ID = 11,
    GW_KEY_SEQUENCE = 12,
    GW_KEY_TOTAL = 13,
    GW_KEY_VALUE_TYPE = 14,
    GW_KEY_CAPABILITY_FLAGS = 15,
    GW_KEY_MIN_VALUE = 16,
    GW_KEY_MAX_VALUE = 17,
    GW_KEY_STEP = 18,
    GW_KEY_CAPABILITY_LABEL = 19,
    GW_KEY_CAPABILITY_UNIT = 20,
    GW_KEY_CAPABILITY_REVISION = 21,
    GW_KEY_FEATURE_ID = 22,
    GW_KEY_FEATURE_TYPE = 23,
    GW_KEY_FEATURE_SCHEMA_VERSION = 24,
    GW_KEY_FEATURE_FLAGS = 25,
    GW_KEY_PROPERTY_ID = 26,
    GW_KEY_FEATURE_VALUE_BOOL = 27,
    GW_KEY_FEATURE_VALUE_INT = 28,
    GW_KEY_FEATURE_TOOL = 29,
    GW_KEY_FEATURE_TOTAL = 30,
    GW_KEY_FEATURE_DECIMALS = 31,

    /* Settings v2 keys (Phase 0 — additive, backward-compatible). */
    GW_KEY_SETTINGS_SUPPORTED = 32,
    GW_KEY_SETTINGS_SCHEMA_REVISION = 33,
    GW_KEY_SETTINGS_ID = 34,
    GW_KEY_SETTINGS_TITLE = 35,
    GW_KEY_SETTINGS_GROUP = 36,
    GW_KEY_SETTINGS_UNIT = 37,
    GW_KEY_SETTINGS_TYPE = 38,
    GW_KEY_SETTINGS_FLAGS = 39,
    GW_KEY_SETTINGS_VALUE = 40,
    GW_KEY_SETTINGS_TRANSACTION_ID = 41,
    GW_KEY_SETTINGS_EXPECTED_REVISION = 42,
    GW_KEY_SETTINGS_NEW_REVISION = 43,
    GW_KEY_SETTINGS_OPTION_INDEX = 44,
    GW_KEY_SETTINGS_MAX_LENGTH = 45,
    GW_KEY_SETTINGS_OPTION_COUNT = 46,
    GW_KEY_SETTINGS_SEQUENCE = 47,
};

typedef enum {
    GW_FEATURE_NONE = 0,
    GW_FEATURE_GENERIC_RELAY = 1,
    GW_FEATURE_GENERIC_VALUE = 2,
    GW_FEATURE_ON_OFF_PLUGIN_UNIT = 10,
    GW_FEATURE_ON_OFF_LIGHT = 11,
    GW_FEATURE_DIMMABLE_LIGHT = 12,
    GW_FEATURE_FAN = 20,
    GW_FEATURE_TEMPERATURE_SENSOR = 30,
    GW_FEATURE_HUMIDITY_SENSOR = 31,
    GW_FEATURE_CONTACT_SENSOR = 40,
} gw_feature_type_t;

typedef enum {
    GW_PROP_NONE = 0,
    GW_PROP_ON_OFF = 1,
    GW_PROP_LEVEL = 2,
    GW_PROP_PERCENT_SETTING = 3,
    GW_PROP_PERCENT_CURRENT = 4,
    GW_PROP_TEMPERATURE = 5,
    GW_PROP_HUMIDITY = 6,
    GW_PROP_CONTACT = 7,
    GW_PROP_VALUE = 8,
} gw_feature_property_t;

/* Settings v2 type and flag constants (Phase 0). */
typedef enum {
    GW_SETTING_TYPE_NONE   = 0,
    GW_SETTING_TYPE_BOOL   = 1,
    GW_SETTING_TYPE_INT    = 2,
    GW_SETTING_TYPE_FLOAT  = 3,
    GW_SETTING_TYPE_STRING = 4,
    GW_SETTING_TYPE_ENUM   = 5,
} gw_setting_type_t;

#define GW_SETTING_TYPE_BOOL_LEGACY   0
#define GW_SETTING_TYPE_INT_LEGACY    1
#define GW_SETTING_TYPE_STRING_LEGACY 2
#define GW_SETTING_TYPE_ENUM_LEGACY   3

enum {
    GW_SETTING_FLAG_READONLY  = 1u << 0,
    GW_SETTING_FLAG_SECRET    = 1u << 1,
    GW_SETTING_FLAG_ADVANCED  = 1u << 2,
};

/* ------------------------------------------------------------------ *
 * Result codes
 * ------------------------------------------------------------------ */

typedef enum {
    GW_OK = 0,
    GW_ERR_INVALID_ARG = -1,
    GW_ERR_NO_SPACE = -2,
    GW_ERR_ENCODE = -3,
    GW_ERR_DECODE = -4,
    GW_ERR_UNSUPPORTED_VERSION = -5,
    GW_ERR_VALIDATION = -6,
} gw_result_t;

const char *gw_result_str(int result);

/* ------------------------------------------------------------------ *
 * Message model (mirrors Gateway gw_message_t)
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t protocol_version;
    char type[GW_MSG_TYPE_LEN];
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    uint32_t request_id;
    int has_request_id;
    int int_value;
    int bool_value;
    int has_int_value;
    int has_bool_value;
    int has_device_id;
    char name[GW_MSG_NAME_LEN];
    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    int has_ble_addr;
    uint32_t snapshot_id;
    int has_snapshot_id;
    uint16_t sequence;
    int has_sequence;
    uint16_t total;
    int has_total;
    uint8_t value_type;
    int has_value_type;
    uint8_t capability_flags;
    int has_capability_flags;
    int32_t min_value;
    int has_min_value;
    int32_t max_value;
    int has_max_value;
    uint32_t step;
    int has_step;
    char capability_label[GW_MSG_CAP_LABEL_LEN];
    char capability_unit[GW_MSG_CAP_UNIT_LEN];
    uint32_t capability_revision;
    int has_capability_revision;
    char feature_id[GW_FEATURE_ID_LEN];
    int has_feature_id;
    uint8_t feature_type;
    int has_feature_type;
    uint16_t feature_schema_version;
    int has_feature_schema_version;
    uint16_t feature_flags;
    int has_feature_flags;
    uint8_t property_id;
    int has_property_id;
    bool feature_value_bool;
    int has_feature_value_bool;
    int32_t feature_value_int;
    int has_feature_value_int;
    char feature_tool[GW_MSG_COMMAND_LEN];
    int has_feature_tool;
    uint16_t feature_total;
    int has_feature_total;
    uint8_t feature_decimals;
    int has_feature_decimals;

    /* Settings v2 fields (Phase 0 — additive, backward-compatible). */
    int has_settings_supported;
    int settings_supported;
    uint16_t settings_schema_revision;
    int has_settings_schema_revision;
    char setting_id[GW_FEATURE_ID_LEN];
    int has_setting_id;
    char setting_title[GW_MSG_CAP_LABEL_LEN];
    int has_setting_title;
    char setting_group[32];
    int has_setting_group;
    char setting_unit[GW_MSG_CAP_UNIT_LEN];
    int has_setting_unit;
    uint8_t setting_type;
    int has_setting_type;
    uint16_t setting_flags;
    int has_setting_flags;
    uint16_t setting_max_length;
    int has_setting_max_length;
    uint8_t setting_option_count;
    int has_setting_option_count;
    uint8_t setting_option_index;
    int has_setting_option_index;
    uint64_t settings_transaction_id;
    int has_settings_transaction_id;
    uint32_t settings_expected_revision;
    int has_settings_expected_revision;
    uint32_t settings_new_revision;
    int has_settings_new_revision;
} gw_message_t;

/* Zero-init a message; TX then defaults to emitting protocol v4. */
void gw_message_init(gw_message_t *msg);

/* Max ATT payload for notify/write given negotiated MTU:
 * min(GW_MSG_MAX_LEN, negotiated_mtu - 3). Never assume MTU == 256. */
uint16_t gw_ble_max_tx_payload(uint16_t negotiated_mtu);

/* ------------------------------------------------------------------ *
 * Codec
 * ------------------------------------------------------------------ */

/* Encode msg as CBOR map into out_buf.
 * Always emits: protocol_version (must equal GW_PROTOCOL_VERSION), type,
 * command, int_value, bool_value.
 * Emits optionally: device_id, request_id, name, ble_addr(+ble_addr_type),
 * capability metadata and v4 semantic feature metadata/value fields.
 * Key 7 is reserved and never emitted.
 * Returns encoded length (>0) or negative gw_result_t. */
int gw_message_encode(const gw_message_t *msg, uint8_t *out_buf,
                      size_t out_cap);

/* Decode CBOR map into out_msg. Strict against current Gateway decoder:
 * requires protocol_version == GW_PROTOCOL_VERSION (absent or any other
 * version is rejected), type, command, int_value, bool_value;
 * request_id if present in 1..UINT32_MAX; reserved key 7 is ignored;
 * total length <= GW_MSG_MAX_LEN; no trailing bytes.
 * Returns GW_OK or negative gw_result_t. */
int gw_message_decode(const uint8_t *buf, size_t len, gw_message_t *out_msg);

/* ------------------------------------------------------------------ *
 * Builders + TX validation (encode the ACK/event contract)
 * ------------------------------------------------------------------ */

/* Build device_ack echoing request identity per contract:
 * exact request_id, exact command, logical device_id.
 * success -> bool_value = true/false; state/result goes to int_value. */
void gw_build_ack(gw_message_t *ack, const gw_message_t *request,
                  const char *device_id, bool success, int int_value);

/* Build device_event. command carries the event name. */
void gw_build_event(gw_message_t *event, const char *device_id,
                    const char *event_name, int int_value, bool bool_value);
void gw_build_feature_event_bool(gw_message_t *event, const char *device_id,
                                 const char *feature_id, uint8_t property_id,
                                 bool value);
void gw_build_feature_event_int(gw_message_t *event, const char *device_id,
                                const char *feature_id, uint8_t property_id,
                                int32_t value);

/* True if msg satisfies mandatory Gateway-side rules for its type
 * (ACK: non-empty device_id/command, request_id >= 1;
 * EVENT: non-empty command). Run before notify submission. */
bool gw_message_valid_ack(const gw_message_t *msg);
bool gw_message_valid_event(const gw_message_t *msg);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_PROTOCOL_H */
