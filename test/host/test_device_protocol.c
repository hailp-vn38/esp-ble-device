/*
 * Host unit tests for Device protocol (spec §23..#31).
 *
 * Tests:
 *   DEV-PROTO-001 — Gateway command decode
 *   DEV-PROTO-002 — Device ACK encode
 *   DEV-PROTO-003 — malformed request rejection
 *   DEV-PROTO-004 — capability golden vectors
 *   DEV-PROTO-005 — roundtrip verification
 *
 * Golden vectors come from ESP_BLE_Gateway_Device_Integration_Contract:
 *   - set_led BOOL command
 *   - get_state NONE command
 *   - device_ack success/failure
 *   - capabilities_begin/item/end
 *
 * Run: test/host/run_device_protocol_tests.sh
 */
#include <stdio.h>
#include <string.h>

#include "gateway_protocol.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
        g_checks++;                                                          \
    } while (0)

#define CHECK_INT(actual, expected)                                          \
    do {                                                                     \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        if (a_ != e_) {                                                      \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s == %lld, got %lld\n", __FILE__,  \
                    __LINE__, #actual, e_, a_);                              \
        }                                                                    \
        g_checks++;                                                          \
    } while (0)

/* ------------------------------------------------------------------ *
 * DEV-PROTO-001 — Gateway command decode
 *
 * Decode actual/golden Gateway v4 set_led and preserve all required fields.
 * ------------------------------------------------------------------ */

static void test_gateway_command_decode(void)
{
    gw_message_t msg, decoded;
    uint8_t buf[GW_MSG_MAX_LEN];

    /* Build a set_led command (simulating Gateway output). */
    gw_message_init(&msg);
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(msg.device_id, "lamp-1");
    msg.has_device_id = 1;
    strcpy(msg.command, "set_led");
    msg.request_id = 42;
    msg.has_request_id = 1;
    msg.bool_value = 1;
    msg.int_value = 0;

    /* Encode and decode. */
    int encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);

    /* Verify all required fields preserved. */
    CHECK(strcmp(decoded.type, GW_MSG_TYPE_DEVICE_COMMAND) == 0);
    CHECK(strcmp(decoded.device_id, "lamp-1") == 0);
    CHECK(decoded.has_device_id == 1);
    CHECK(strcmp(decoded.command, "set_led") == 0);
    CHECK(decoded.request_id == 42);
    CHECK(decoded.has_request_id == 1);
    CHECK(decoded.bool_value == 1);
    CHECK(decoded.int_value == 0);
    CHECK(decoded.protocol_version == GW_PROTOCOL_VERSION);

    /* Build a get_state command. */
    gw_message_init(&msg);
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(msg.device_id, "lamp-1");
    msg.has_device_id = 1;
    strcpy(msg.command, "get_state");
    msg.request_id = 99;
    msg.has_request_id = 1;
    msg.bool_value = 0;

    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(strcmp(decoded.type, GW_MSG_TYPE_DEVICE_COMMAND) == 0);
    CHECK(strcmp(decoded.device_id, "lamp-1") == 0);
    CHECK(strcmp(decoded.command, "get_state") == 0);
    CHECK(decoded.request_id == 99);
    CHECK(decoded.bool_value == 0);

    printf("DEV-PROTO-001: Gateway command decode passed\n");
}

/* ------------------------------------------------------------------ *
 * DEV-PROTO-002 — Device ACK encode
 *
 * Gateway decoder/golden expectation sees exact fields.
 * ------------------------------------------------------------------ */

static void test_device_ack_encode(void)
{
    gw_message_t request, ack;
    uint8_t buf[GW_MSG_MAX_LEN];
    gw_message_t decoded;

    /* Build ACK for set_led success. */
    gw_message_init(&request);
    request.protocol_version = GW_PROTOCOL_VERSION;
    strcpy(request.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(request.device_id, "lamp-1");
    request.has_device_id = 1;
    strcpy(request.command, "set_led");
    request.request_id = 42;
    request.has_request_id = 1;

    gw_build_ack(&ack, &request, request.device_id, true, 1);
    CHECK(gw_message_valid_ack(&ack));

    /* Encode and decode. */
    int encoded = gw_message_encode(&ack, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);

    /* Verify all ACK fields. */
    CHECK(strcmp(decoded.type, GW_MSG_TYPE_DEVICE_ACK) == 0);
    CHECK(strcmp(decoded.device_id, "lamp-1") == 0);
    CHECK(strcmp(decoded.command, "set_led") == 0);
    CHECK(decoded.request_id == 42);
    CHECK(decoded.has_request_id == 1);
    CHECK(decoded.bool_value == 1);
    CHECK(decoded.int_value == 1);

    /* Build ACK for failure. */
    gw_build_ack(&ack, &request, request.device_id, false, 0);
    encoded = gw_message_encode(&ack, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(strcmp(decoded.type, GW_MSG_TYPE_DEVICE_ACK) == 0);
    CHECK(decoded.bool_value == 0);
    CHECK(decoded.int_value == 0);

    printf("DEV-PROTO-002: Device ACK encode passed\n");
}

/* ------------------------------------------------------------------ *
 * DEV-PROTO-003 — malformed request rejection
 *
 * Bad request ID/type/required fields rejected deterministically.
 * ------------------------------------------------------------------ */

static void test_malformed_request_rejection(void)
{
    gw_message_t decoded;

    /* Missing type field. */
    static const uint8_t NO_TYPE[] = {
        0xA6, 0x00, 0x04, 0x02, 0x66, 'l', 'a', 'm', 'p', '-', '1',
        0x03, 0x67, 's', 'e', 't', '_', 'l', 'e', 'd',
        0x04, 0x00, 0x05, 0xF5, 0x0A, 0x01,
    };
    CHECK_INT(gw_message_decode(NO_TYPE, sizeof(NO_TYPE), &decoded),
              GW_ERR_DECODE);

    /* Missing command field. */
    static const uint8_t NO_COMMAND[] = {
        0xA6, 0x00, 0x04, 0x01, 0x6E, 'd', 'e', 'v', 'i', 'c', 'e', '_',
        'c',  'o',  'm',  'm', 'a', 'n', 'd',
        0x02, 0x66, 'l', 'a', 'm', 'p', '-', '1',
        0x04, 0x00, 0x05, 0xF5, 0x0A, 0x01,
    };
    CHECK_INT(gw_message_decode(NO_COMMAND, sizeof(NO_COMMAND), &decoded),
              GW_ERR_DECODE);

    /* request_id = 0 (invalid per spec). */
    static const uint8_t REQUEST_ZERO[] = {
        0xA7, 0x00, 0x04, 0x01, 0x6E, 'd', 'e', 'v', 'i', 'c', 'e', '_',
        'c',  'o',  'm',  'm', 'a', 'n', 'd',
        0x02, 0x66, 'l', 'a', 'm', 'p', '-', '1',
        0x03, 0x67, 's', 'e', 't', '_', 'l', 'e', 'd',
        0x04, 0x00, 0x05, 0xF5, 0x0A, 0x00,
    };
    CHECK_INT(gw_message_decode(REQUEST_ZERO, sizeof(REQUEST_ZERO), &decoded),
              GW_ERR_DECODE);

    /* Protocol version 0 (invalid). */
    static const uint8_t VERSION_0[] = {
        0xA7, 0x00, 0x00, 0x01, 0x6E, 'd', 'e', 'v', 'i', 'c', 'e', '_',
        'c',  'o',  'm',  'm', 'a', 'n', 'd',
        0x02, 0x66, 'l', 'a', 'm', 'p', '-', '1',
        0x03, 0x67, 's', 'e', 't', '_', 'l', 'e', 'd',
        0x04, 0x00, 0x05, 0xF5, 0x0A, 0x01,
    };
    CHECK_INT(gw_message_decode(VERSION_0, sizeof(VERSION_0), &decoded),
              GW_ERR_UNSUPPORTED_VERSION);

    /* Oversized payload. */
    static uint8_t oversized[GW_MSG_MAX_LEN + 1];
    memset(oversized, 0xA1, sizeof(oversized));
    CHECK_INT(gw_message_decode(oversized, sizeof(oversized), &decoded),
              GW_ERR_INVALID_ARG);

    printf("DEV-PROTO-003: malformed request rejection passed\n");
}

/* ------------------------------------------------------------------ *
 * DEV-PROTO-004 — capability golden vectors
 *
 * Verify capability messages encode/decode correctly.
 * ------------------------------------------------------------------ */

static void test_capability_golden_vectors(void)
{
    gw_message_t msg, decoded;
    uint8_t buf[GW_MSG_MAX_LEN];

    /* capabilities_begin. */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_CAPABILITIES_BEGIN);
    strcpy(msg.device_id, "lamp-1");
    msg.has_device_id = 1;
    strcpy(msg.command, GW_COMMAND_DESCRIBE_CAPABILITIES);
    msg.snapshot_id = 1;
    msg.has_snapshot_id = 1;
    msg.total = 2;
    msg.has_total = 1;
    msg.capability_revision = 1;
    msg.has_capability_revision = 1;

    int encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(strcmp(decoded.type, GW_MSG_TYPE_CAPABILITIES_BEGIN) == 0);
    CHECK(strcmp(decoded.device_id, "lamp-1") == 0);
    CHECK(strcmp(decoded.command, GW_COMMAND_DESCRIBE_CAPABILITIES) == 0);
    CHECK_INT(decoded.snapshot_id, 1);
    CHECK_INT(decoded.total, 2);
    CHECK_INT(decoded.capability_revision, 1);

    /* capability_item set_led BOOL. */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_CAPABILITY_ITEM);
    strcpy(msg.device_id, "lamp-1");
    msg.has_device_id = 1;
    strcpy(msg.command, "set_led");
    msg.snapshot_id = 1;
    msg.has_snapshot_id = 1;
    msg.sequence = 0;
    msg.has_sequence = 1;
    msg.value_type = 1; /* BOOL */
    msg.has_value_type = 1;
    msg.capability_flags = 1; /* IDEMPOTENT */
    msg.has_capability_flags = 1;
    strcpy(msg.capability_label, "LED power");
    strcpy(msg.capability_unit, "");

    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(strcmp(decoded.type, GW_MSG_TYPE_CAPABILITY_ITEM) == 0);
    CHECK(strcmp(decoded.command, "set_led") == 0);
    CHECK_INT(decoded.sequence, 0);
    CHECK_INT(decoded.value_type, 1);
    CHECK_INT(decoded.capability_flags, 1);
    CHECK(strcmp(decoded.capability_label, "LED power") == 0);

    /* capability_item get_state NONE. */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_CAPABILITY_ITEM);
    strcpy(msg.device_id, "lamp-1");
    msg.has_device_id = 1;
    strcpy(msg.command, "get_state");
    msg.snapshot_id = 1;
    msg.has_snapshot_id = 1;
    msg.sequence = 1;
    msg.has_sequence = 1;
    msg.value_type = 0; /* NONE */
    msg.has_value_type = 1;
    msg.capability_flags = 1; /* IDEMPOTENT */
    msg.has_capability_flags = 1;
    strcpy(msg.capability_label, "LED state");
    strcpy(msg.capability_unit, "");

    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(strcmp(decoded.command, "get_state") == 0);
    CHECK_INT(decoded.sequence, 1);
    CHECK_INT(decoded.value_type, 0);
    CHECK(strcmp(decoded.capability_label, "LED state") == 0);

    /* capabilities_end. */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_CAPABILITIES_END);
    strcpy(msg.device_id, "lamp-1");
    msg.has_device_id = 1;
    strcpy(msg.command, GW_COMMAND_DESCRIBE_CAPABILITIES);
    msg.snapshot_id = 1;
    msg.has_snapshot_id = 1;
    msg.total = 2;
    msg.has_total = 1;
    msg.bool_value = 1;

    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(strcmp(decoded.type, GW_MSG_TYPE_CAPABILITIES_END) == 0);
    CHECK_INT(decoded.total, 2);
    CHECK(decoded.bool_value == 1);

    printf("DEV-PROTO-004: capability golden vectors passed\n");
}

/* ------------------------------------------------------------------ *
 * DEV-PROTO-005 — roundtrip verification
 *
 * Encode -> Decode -> verify all fields preserved.
 * ------------------------------------------------------------------ */

static void test_roundtrip_verification(void)
{
    gw_message_t msg, decoded;
    uint8_t buf[GW_MSG_MAX_LEN];

    /* ACK roundtrip. */
    gw_message_t request;
    gw_message_init(&request);
    request.protocol_version = GW_PROTOCOL_VERSION;
    strcpy(request.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(request.device_id, "lamp-1");
    request.has_device_id = 1;
    strcpy(request.command, "set_led");
    request.request_id = 42;
    request.has_request_id = 1;

    gw_build_ack(&msg, &request, request.device_id, true, 1);
    int encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(strcmp(decoded.type, GW_MSG_TYPE_DEVICE_ACK) == 0);
    CHECK(strcmp(decoded.device_id, "lamp-1") == 0);
    CHECK(strcmp(decoded.command, "set_led") == 0);
    CHECK(decoded.request_id == 42);
    CHECK(decoded.bool_value == 1);
    CHECK(decoded.int_value == 1);

    /* Event roundtrip. */
    gw_build_event(&msg, "esp32s3-ref", "button_pressed", 1, true);
    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(strcmp(decoded.type, GW_MSG_TYPE_DEVICE_EVENT) == 0);
    CHECK(strcmp(decoded.device_id, "esp32s3-ref") == 0);
    CHECK(strcmp(decoded.command, "button_pressed") == 0);
    CHECK(decoded.int_value == 1);
    CHECK(decoded.bool_value == 1);

    /* Capability item roundtrip. */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_CAPABILITY_ITEM);
    strcpy(msg.device_id, "lamp-1");
    msg.has_device_id = 1;
    strcpy(msg.command, "set_brightness");
    msg.snapshot_id = 1;
    msg.has_snapshot_id = 1;
    msg.sequence = 0;
    msg.has_sequence = 1;
    msg.value_type = 2; /* INT */
    msg.has_value_type = 1;
    msg.min_value = 0;
    msg.has_min_value = 1;
    msg.max_value = 100;
    msg.has_max_value = 1;
    msg.step = 5;
    msg.has_step = 1;
    strcpy(msg.capability_label, "Brightness");
    strcpy(msg.capability_unit, "%");

    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(strcmp(decoded.command, "set_brightness") == 0);
    CHECK_INT(decoded.value_type, 2);
    CHECK_INT(decoded.min_value, 0);
    CHECK_INT(decoded.max_value, 100);
    CHECK_INT(decoded.step, 5);
    CHECK(strcmp(decoded.capability_label, "Brightness") == 0);
    CHECK(strcmp(decoded.capability_unit, "%") == 0);

    printf("DEV-PROTO-005: roundtrip verification passed\n");
}

int main(void)
{
    test_gateway_command_decode();
    test_device_ack_encode();
    test_malformed_request_rejection();
    test_capability_golden_vectors();
    test_roundtrip_verification();

    printf("\ndevice_protocol: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
