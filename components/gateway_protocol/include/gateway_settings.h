/*
 * gateway_settings — Settings v2 specialized codec (Phase 0).
 *
 * Specialized encode/decode for Settings message families, keeping
 * the shared gw_message_t lean. All encoded frames are bounded by
 * GW_MSG_MAX_LEN.
 *
 * This file is additive and backward-compatible with Protocol v4.
 */
#ifndef GATEWAY_SETTINGS_H
#define GATEWAY_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gateway_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Settings message families (Phase 0.4 — locked)
 *
 * Discovery:
 *   describe_settings (request)
 *   settings_begin / setting_item / setting_option_item / settings_end (response)
 *   read_settings (request)
 *   settings_values_begin / setting_value / settings_values_end (response)
 *
 * Transaction:
 *   settings_tx_begin / settings_tx_set / settings_tx_commit / settings_tx_abort
 *   settings_commit_confirm
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Encode API — streaming, bounded by GW_MSG_MAX_LEN
 * ------------------------------------------------------------------ */

/* Encode settings_begin frame (schema discovery response header).
 * @param sequence     Current sequence number for this stream.
 * @param total        Total number of settings in this device.
 * @param request_id   Echo of request's request_id (if present).
 * @param out_buf      Output buffer (must be >= GW_MSG_MAX_LEN).
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_begin(uint8_t *out_buf, size_t out_cap,
                             uint16_t total, uint32_t request_id);

/* Encode setting_item frame (one setting descriptor).
 * @param item_index   Zero-based index of this setting.
 * @param total        Total number of settings.
 * @param request_id   Echo of request's request_id (if present).
 * @param setting_id   Unique setting identifier (e.g. "wifi_ssid").
 * @param title        Human-readable title (e.g. "WiFi SSID").
 * @param group        Group name (e.g. "network") or "" for ungrouped.
 * @param unit         Unit string (e.g. "dBm") or "" for unitless.
 * @param type         Setting type (GW_SETTING_TYPE_BOOL/INT/STRING/ENUM).
 * @param flags        Setting flags (GW_SETTING_FLAG_*).
 * @param max_length   For STRING: max character count. 0 if not applicable.
 * @param min_value    For INT: minimum value. 0 if not applicable.
 * @param max_value    For INT: maximum value. 0 if not applicable.
 * @param step         For INT: step size. 0 if not applicable.
 * @param out_buf      Output buffer.
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_item(uint8_t *out_buf, size_t out_cap,
                            uint16_t item_index, uint16_t total,
                            uint32_t request_id,
                            const char *setting_id,
                            const char *title,
                            const char *group,
                            const char *unit,
                            uint8_t type,
                            uint16_t flags,
                            uint16_t max_length,
                            int32_t min_value,
                            int32_t max_value,
                            uint32_t step);

/* Encode setting_option_item frame (one enum option for a setting).
 * @param item_index   Zero-based index of the parent setting.
 * @param option_index Zero-based index of this option.
 * @param request_id   Echo of request's request_id (if present).
 * @param option_label Option display label.
 * @param out_buf      Output buffer.
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_option_item(uint8_t *out_buf, size_t out_cap,
                                   uint16_t item_index,
                                   uint8_t option_index,
                                   uint32_t request_id,
                                   const char *option_label);

/* Encode settings_end frame (end of schema discovery stream).
 * @param total        Total number of settings sent.
 * @param request_id   Echo of request's request_id (if present).
 * @param out_buf      Output buffer.
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_end(uint8_t *out_buf, size_t out_cap,
                           uint16_t total, uint32_t request_id);

/* Encode settings_values_begin frame (values discovery response header).
 * @param total        Total number of settings.
 * @param revision     Current config_revision.
 * @param request_id   Echo of request's request_id (if present).
 * @param out_buf      Output buffer.
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_values_begin(uint8_t *out_buf, size_t out_cap,
                                    uint16_t total, uint32_t revision,
                                    uint32_t request_id);

/* Encode setting_value frame (current value for one setting).
 * @param item_index   Zero-based index of this setting.
 * @param setting_id   Setting identifier.
 * @param value_type   Value type (GW_SETTING_TYPE_BOOL/INT/STRING/ENUM).
 * @param value        Union: bool/int32/string pointer/enum index.
 * @param request_id   Echo of request's request_id (if present).
 * @param out_buf      Output buffer.
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_value(uint8_t *out_buf, size_t out_cap,
                             uint16_t item_index,
                             const char *setting_id,
                             uint8_t value_type,
                             const void *value,
                             uint32_t request_id);

/* Encode settings_values_end frame (end of values discovery stream).
 * @param total        Total number of settings sent.
 * @param revision     Current config_revision.
 * @param request_id   Echo of request's request_id (if present).
 * @param out_buf      Output buffer.
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_values_end(uint8_t *out_buf, size_t out_cap,
                                  uint16_t total, uint32_t revision,
                                  uint32_t request_id);

/* ------------------------------------------------------------------ *
 * Transaction encode API
 * ------------------------------------------------------------------ */

/* Encode settings_tx_begin frame (start transaction).
 * @param transaction_id  Unique transaction identifier.
 * @param expected_revision  Expected config_revision for optimistic locking.
 * @param request_id   Echo of request's request_id.
 * @param out_buf      Output buffer.
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_tx_begin(uint8_t *out_buf, size_t out_cap,
                                uint64_t transaction_id,
                                uint32_t expected_revision,
                                uint32_t request_id);

/* Encode settings_tx_set frame (set a single setting value).
 * @param transaction_id  Transaction identifier.
 * @param setting_id   Setting to modify.
 * @param value_type   Value type (BOOL/INT/STRING/ENUM).
 * @param value        New value.
 * @param request_id   Echo of request's request_id.
 * @param out_buf      Output buffer.
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_tx_set(uint8_t *out_buf, size_t out_cap,
                              uint64_t transaction_id,
                              const char *setting_id,
                              uint8_t value_type,
                              const void *value,
                              uint32_t request_id);

/* Encode settings_tx_commit frame (commit all staged changes).
 * @param transaction_id  Transaction identifier.
 * @param request_id   Echo of request's request_id.
 * @param out_buf      Output buffer.
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_tx_commit(uint8_t *out_buf, size_t out_cap,
                                 uint64_t transaction_id,
                                 uint32_t request_id);

/* Encode settings_tx_abort frame (abort transaction, discard staging).
 * @param transaction_id  Transaction identifier.
 * @param request_id   Echo of request's request_id.
 * @param out_buf      Output buffer.
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_tx_abort(uint8_t *out_buf, size_t out_cap,
                                uint64_t transaction_id,
                                uint32_t request_id);

/* Encode settings_commit_confirm frame (device confirms commit + reboot).
 * @param transaction_id  Transaction identifier.
 * @param new_revision    New config_revision after commit.
 * @param request_id   Echo of request's request_id.
 * @param out_buf      Output buffer.
 * @param out_cap      Capacity of out_buf.
 * @return             Encoded length (>0) or negative gw_result_t. */
int gw_settings_encode_commit_confirm(uint8_t *out_buf, size_t out_cap,
                                      uint64_t transaction_id,
                                      uint32_t new_revision,
                                      uint32_t request_id);

/* ------------------------------------------------------------------ *
 * Decode API — for Settings command processing
 * ------------------------------------------------------------------ */

/* Parsed Settings command type. */
typedef enum {
    GW_SETTINGS_CMD_NONE = 0,
    GW_SETTINGS_CMD_DESCRIBE,
    GW_SETTINGS_CMD_READ,
    GW_SETTINGS_CMD_TX_BEGIN,
    GW_SETTINGS_CMD_TX_SET,
    GW_SETTINGS_CMD_TX_COMMIT,
    GW_SETTINGS_CMD_TX_ABORT,
    GW_SETTINGS_CMD_TX_CONFIRM,
} gw_settings_cmd_type_t;

/* Parsed Settings value (for TX_SET). */
typedef struct {
    uint8_t type;        /* GW_SETTING_TYPE_* */
    union {
        bool bool_val;
        int32_t int_val;
        char str_val[GW_SETTINGS_VALUE_STR_LEN];
        uint8_t enum_val;
    } value;
} gw_settings_value_t;

/* Decoded Settings command context. */
typedef struct {
    gw_settings_cmd_type_t cmd_type;
    uint32_t request_id;
    int has_request_id;
    uint64_t transaction_id;
    int has_transaction_id;
    uint32_t expected_revision;
    int has_expected_revision;
    uint32_t new_revision;
    int has_new_revision;
    char setting_id[GW_FEATURE_ID_LEN];
    int has_setting_id;
    gw_settings_value_t setting_value;
    int has_setting_value;
} gw_settings_command_t;

/* Decode a Settings command from a device_command message.
 * Extracts Settings-specific fields from the raw gw_message_t.
 * @param msg          Decoded gateway message (type=device_command).
 * @param out_cmd      Output: parsed Settings command.
 * @return             GW_OK or negative gw_result_t. */
int gw_settings_decode_command(const gw_message_t *msg,
                               gw_settings_command_t *out_cmd);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_SETTINGS_H */