/*
 * Cross-interop verification against the REAL Gateway codec stack:
 *
 *   Direction A: gw_message_encode() -> Gateway-style QCBOR decode
 *   Direction B: Gateway-style QCBOR encode -> gw_message_decode()
 *
 * The QCBOR call sequences mirror
 * esp-ble-gateway/components/cbor_codec/cbor_codec.c exactly.
 *
 * Run manually:
 *   sh test/host/run_gateway_interop_check.sh /path/to/esp-ble-gateway
 */
#include <stdio.h>
#include <string.h>

#include "qcbor/qcbor_decode.h"
#include "qcbor/qcbor_encode.h"
#include "qcbor/qcbor_spiffy_decode.h"

#include "gateway_protocol.h"

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            g_failures++;                                               \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #cond);          \
        }                                                               \
    } while (0)

/* ----------------------------------------------------------------- *
 * Gateway-side decode (mirror of cbor_codec_decode targeted lookups)
 * ----------------------------------------------------------------- */
static int gateway_style_decode(const uint8_t *buf, size_t len,
                                gw_message_t *out)
{
    memset(out, 0, sizeof(*out));

    QCBORDecodeContext ctx;
    QCBORDecode_Init(&ctx, (UsefulBufC){buf, len}, QCBOR_DECODE_MODE_NORMAL);
    QCBORDecode_EnterMap(&ctx, NULL);
    if (QCBORDecode_GetAndResetError(&ctx) != QCBOR_SUCCESS) return -1;

    uint64_t version = GW_PROTOCOL_VERSION;
    QCBORDecode_GetUInt64InMapN(&ctx, 0, &version);
    QCBORError err = QCBORDecode_GetAndResetError(&ctx);
    if (err != QCBOR_SUCCESS && err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;
    if (version == 0 || version > GW_PROTOCOL_VERSION) return -1;

    UsefulBufC text;
    QCBORDecode_GetTextStringInMapN(&ctx, 1, &text);
    if (QCBORDecode_GetAndResetError(&ctx) != QCBOR_SUCCESS ||
        text.len >= sizeof(out->type) || text.len == 0) {
        return -1;
    }
    memcpy(out->type, text.ptr, text.len);
    out->type[text.len] = '\0';

    int64_t int_value = 0;
    bool bool_value = false;

    QCBORDecode_GetTextStringInMapN(&ctx, 3, &text);
    if (QCBORDecode_GetAndResetError(&ctx) != QCBOR_SUCCESS ||
        text.len >= sizeof(out->command) || text.len == 0) {
        return -1;
    }
    memcpy(out->command, text.ptr, text.len);
    out->command[text.len] = '\0';

    QCBORDecode_GetInt64InMapN(&ctx, 4, &int_value);
    if (QCBORDecode_GetAndResetError(&ctx) != QCBOR_SUCCESS) return -1;

    QCBORDecode_GetBoolInMapN(&ctx, 5, &bool_value);
    if (QCBORDecode_GetAndResetError(&ctx) != QCBOR_SUCCESS) return -1;

    uint64_t request_id = 0;
    QCBORDecode_GetUInt64InMapN(&ctx, 10, &request_id);
    err = QCBORDecode_GetAndResetError(&ctx);
    if (err == QCBOR_SUCCESS) {
        if (request_id == 0 || request_id > UINT32_MAX) return -1;
        out->request_id = (uint32_t)request_id;
        out->has_request_id = 1;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    UsefulBufC device_id_text;
    QCBORDecode_GetTextStringInMapN(&ctx, 2, &device_id_text);
    err = QCBORDecode_GetAndResetError(&ctx);
    if (err == QCBOR_SUCCESS) {
        if (device_id_text.len >= sizeof(out->device_id) ||
            device_id_text.len == 0) {
            return -1;
        }
        memcpy(out->device_id, device_id_text.ptr, device_id_text.len);
        out->device_id[device_id_text.len] = '\0';
        out->has_device_id = 1;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    uint64_t optional_uint = 0;
    QCBORDecode_GetUInt64InMapN(&ctx, 11, &optional_uint);
    err = QCBORDecode_GetAndResetError(&ctx);
    if (err == QCBOR_SUCCESS) {
        out->snapshot_id = (uint32_t)optional_uint;
        out->has_snapshot_id = 1;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;
    QCBORDecode_GetUInt64InMapN(&ctx, 12, &optional_uint);
    err = QCBORDecode_GetAndResetError(&ctx);
    if (err == QCBOR_SUCCESS) {
        out->sequence = (uint16_t)optional_uint;
        out->has_sequence = 1;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;
    QCBORDecode_GetUInt64InMapN(&ctx, 14, &optional_uint);
    err = QCBORDecode_GetAndResetError(&ctx);
    if (err == QCBOR_SUCCESS) {
        out->value_type = (uint8_t)optional_uint;
        out->has_value_type = 1;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;
    QCBORDecode_GetUInt64InMapN(&ctx, 15, &optional_uint);
    err = QCBORDecode_GetAndResetError(&ctx);
    if (err == QCBOR_SUCCESS) {
        out->capability_flags = (uint8_t)optional_uint;
        out->has_capability_flags = 1;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;
    int64_t optional_int = 0;
    QCBORDecode_GetInt64InMapN(&ctx, 16, &optional_int);
    err = QCBORDecode_GetAndResetError(&ctx);
    if (err == QCBOR_SUCCESS) {
        out->min_value = (int32_t)optional_int;
        out->has_min_value = 1;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;
    QCBORDecode_GetInt64InMapN(&ctx, 17, &optional_int);
    err = QCBORDecode_GetAndResetError(&ctx);
    if (err == QCBOR_SUCCESS) {
        out->max_value = (int32_t)optional_int;
        out->has_max_value = 1;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;
    QCBORDecode_GetUInt64InMapN(&ctx, 18, &optional_uint);
    err = QCBORDecode_GetAndResetError(&ctx);
    if (err == QCBOR_SUCCESS) {
        out->step = (uint32_t)optional_uint;
        out->has_step = 1;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;
    QCBORDecode_GetTextStringInMapN(&ctx, 19, &text);
    err = QCBORDecode_GetAndResetError(&ctx);
    if (err == QCBOR_SUCCESS) {
        if (text.len >= sizeof(out->capability_label)) return -1;
        memcpy(out->capability_label, text.ptr, text.len);
        out->capability_label[text.len] = '\0';
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;
    QCBORDecode_GetTextStringInMapN(&ctx, 20, &text);
    err = QCBORDecode_GetAndResetError(&ctx);
    if (err == QCBOR_SUCCESS) {
        if (text.len >= sizeof(out->capability_unit)) return -1;
        memcpy(out->capability_unit, text.ptr, text.len);
        out->capability_unit[text.len] = '\0';
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;
    QCBORDecode_ExitMap(&ctx);
    if (QCBORDecode_Finish(&ctx) != QCBOR_SUCCESS) return -1;

    out->protocol_version = (uint8_t)version;
    out->int_value = (int)int_value;
    out->bool_value = bool_value;
    return 0;
}

/* --------------------------------------------------------------- *
 * Gateway-side encode (mirror of cbor_codec_encode field order)
 * --------------------------------------------------------------- */
static int gateway_style_encode(const gw_message_t *msg, uint8_t *out_buf,
                                size_t cap)
{
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){out_buf, cap});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddUInt64ToMapN(&ctx, 0, msg->protocol_version
                                            ? msg->protocol_version
                                            : GW_PROTOCOL_VERSION);
    QCBOREncode_AddSZStringToMapN(&ctx, 1, msg->type);
    if (msg->has_device_id) {
        QCBOREncode_AddSZStringToMapN(&ctx, 2, msg->device_id);
    }
    QCBOREncode_AddSZStringToMapN(&ctx, 3, msg->command);
    if (msg->has_request_id) {
        QCBOREncode_AddUInt64ToMapN(&ctx, 10, msg->request_id);
    }
    QCBOREncode_AddInt64ToMapN(&ctx, 4, msg->int_value);
    QCBOREncode_AddBoolToMapN(&ctx, 5, msg->bool_value != 0);
    if (msg->name[0] != '\0') {
        QCBOREncode_AddSZStringToMapN(&ctx, 6, msg->name);
    }
    if (msg->device_type[0] != '\0') {
        QCBOREncode_AddSZStringToMapN(&ctx, 7, msg->device_type);
    }
    if (msg->has_ble_addr) {
        QCBOREncode_AddBytesToMapN(&ctx, 8,
                                   (UsefulBufC){msg->ble_addr,
                                                sizeof(msg->ble_addr)});
        QCBOREncode_AddUInt64ToMapN(&ctx, 9, msg->ble_addr_type);
    }
    if (msg->has_snapshot_id) {
        QCBOREncode_AddUInt64ToMapN(&ctx, 11, msg->snapshot_id);
    }
    if (msg->has_sequence) {
        QCBOREncode_AddUInt64ToMapN(&ctx, 12, msg->sequence);
    }
    if (msg->has_total) QCBOREncode_AddUInt64ToMapN(&ctx, 13, msg->total);
    if (msg->has_value_type) {
        QCBOREncode_AddUInt64ToMapN(&ctx, 14, msg->value_type);
    }
    if (msg->has_capability_flags) {
        QCBOREncode_AddUInt64ToMapN(&ctx, 15, msg->capability_flags);
    }
    if (msg->has_min_value) {
        QCBOREncode_AddInt64ToMapN(&ctx, 16, msg->min_value);
    }
    if (msg->has_max_value) {
        QCBOREncode_AddInt64ToMapN(&ctx, 17, msg->max_value);
    }
    if (msg->has_step) QCBOREncode_AddUInt64ToMapN(&ctx, 18, msg->step);
    if (msg->capability_label[0] != '\0') {
        QCBOREncode_AddSZStringToMapN(&ctx, 19, msg->capability_label);
    }
    if (msg->capability_unit[0] != '\0') {
        QCBOREncode_AddSZStringToMapN(&ctx, 20, msg->capability_unit);
    }
    if (msg->has_capability_revision) {
        QCBOREncode_AddUInt64ToMapN(&ctx, 21, msg->capability_revision);
    }
    QCBOREncode_CloseMap(&ctx);

    UsefulBufC encoded;
    if (QCBOREncode_Finish(&ctx, &encoded) != QCBOR_SUCCESS) return -1;
    return (int)encoded.len;
}

/* --------------------------------------------------------------- */

static void verify_fields(const gw_message_t *expected,
                          const gw_message_t *actual)
{
    CHECK(actual->protocol_version == expected->protocol_version);
    CHECK(strcmp(actual->type, expected->type) == 0);
    CHECK(strcmp(actual->command, expected->command) == 0);
    CHECK(actual->int_value == expected->int_value);
    CHECK((actual->bool_value != 0) == (expected->bool_value != 0));
    CHECK(actual->has_request_id == expected->has_request_id);
    if (expected->has_request_id) {
        CHECK(actual->request_id == expected->request_id);
    }
    CHECK(actual->has_device_id == expected->has_device_id);
    if (expected->has_device_id) {
        CHECK(strcmp(actual->device_id, expected->device_id) == 0);
    }
    CHECK(actual->has_snapshot_id == expected->has_snapshot_id);
    if (expected->has_snapshot_id) {
        CHECK(actual->snapshot_id == expected->snapshot_id);
    }
    CHECK(actual->has_sequence == expected->has_sequence);
    if (expected->has_sequence) CHECK(actual->sequence == expected->sequence);
    CHECK(actual->has_value_type == expected->has_value_type);
    if (expected->has_value_type) CHECK(actual->value_type == expected->value_type);
    CHECK(actual->has_min_value == expected->has_min_value);
    if (expected->has_min_value) CHECK(actual->min_value == expected->min_value);
    CHECK(actual->has_max_value == expected->has_max_value);
    if (expected->has_max_value) CHECK(actual->max_value == expected->max_value);
    CHECK(actual->has_step == expected->has_step);
    if (expected->has_step) CHECK(actual->step == expected->step);
    CHECK(strcmp(actual->capability_label, expected->capability_label) == 0);
    CHECK(strcmp(actual->capability_unit, expected->capability_unit) == 0);
}

static void roundtrip(const gw_message_t *msg)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    gw_message_t decoded;

    /* A: Device encoder -> Gateway decoder. */
    int encoded = gw_message_encode(msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK(gateway_style_decode(buf, (size_t)encoded, &decoded) == 0);
    if (g_failures == 0) verify_fields(msg, &decoded);

    /* B: Gateway encoder -> Device decoder. */
    encoded = gateway_style_encode(msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK(gw_message_decode(buf, (size_t)encoded, &decoded) == GW_OK);
    if (g_failures == 0) verify_fields(msg, &decoded);
}

static void sample_ack_failure(gw_message_t *ack)
{
    gw_message_t request;
    gw_message_init(&request);
    strcpy(request.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(request.device_id, "ref_01");
    request.has_device_id = 1;
    strcpy(request.command, "ping");
    request.request_id = 1;
    request.has_request_id = 1;
    gw_build_ack(ack, &request, NULL, false, -2);
}

int main(void)
{
    /* Contract #184 golden ping ACK. */
    gw_message_t request, ack;
    gw_message_init(&request);
    strcpy(request.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(request.device_id, "ref_01");
    request.has_device_id = 1;
    strcpy(request.command, "ping");
    request.request_id = 1;
    request.has_request_id = 1;
    gw_build_ack(&ack, &request, NULL, true, 0);
    roundtrip(&ack);

    sample_ack_failure(&ack);
    roundtrip(&ack);

    /* Event without request_id (#72). */
    gw_message_t event;
    gw_build_event(&event, "relay_01", "button_pressed", 1, true);
    roundtrip(&event);

    /* Negative telemetry value. */
    gw_build_event(&event, "sensor_bedroom", "temperature_mC", -12500, true);
    roundtrip(&event);

    /* Full optional metadata + boundary strings. */
    gw_message_t full;
    gw_message_init(&full);
    strcpy(full.type, GW_MSG_TYPE_DEVICE_EVENT);
    strcpy(full.device_id, "aaaaaaaaaaaaaaaaaaaaaaaa");   /* 24 */
    full.has_device_id = 1;
    strcpy(full.command, "ccccccccccccccccccccccccccccccc"); /* 31 */
    strcpy(full.name, "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn");    /* 31 */
    strcpy(full.device_type, "ttttttttttttttt");             /* 15 */
    full.request_id = 4294967295u;
    full.has_request_id = 1;
    const uint8_t addr[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };
    memcpy(full.ble_addr, addr, sizeof(addr));
    full.ble_addr_type = 0;
    full.has_ble_addr = 1;
    roundtrip(&full);

    /* Protocol-v3 capability item in both wire directions. */
    gw_message_t capability;
    gw_message_init(&capability);
    strcpy(capability.type, GW_MSG_TYPE_CAPABILITY_ITEM);
    strcpy(capability.device_id, "lamp-01");
    capability.has_device_id = 1;
    strcpy(capability.command, "set_brightness");
    capability.snapshot_id = 88;
    capability.has_snapshot_id = 1;
    capability.sequence = 1;
    capability.has_sequence = 1;
    capability.value_type = 2;
    capability.has_value_type = 1;
    capability.capability_flags = 1;
    capability.has_capability_flags = 1;
    capability.min_value = 0;
    capability.has_min_value = 1;
    capability.max_value = 100;
    capability.has_max_value = 1;
    capability.step = 5;
    capability.has_step = 1;
    strcpy(capability.capability_label, "Brightness");
    strcpy(capability.capability_unit, "%");
    roundtrip(&capability);

    printf("interop: %s (%d failures)\n",
           g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
