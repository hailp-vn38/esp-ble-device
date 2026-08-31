/*
 * Host unit tests for components/gateway_protocol.
 *
 * Golden vectors come from ESP_BLE_Gateway_Device_Integration_Contract:
 *   #70/#184 ACK example, #72/#197 event example, #184 golden ping vector,
 *   #185 negative command vector, #187 string-limit table.
 *
 * Run: test/host/run_gateway_protocol_tests.sh
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

static void check_bytes(const uint8_t *actual, size_t actual_len,
                        const uint8_t *expected, size_t expected_len,
                        const char *what, int line)
{
    if (actual_len != expected_len ||
        (actual_len > 0 && memcmp(actual, expected, actual_len) != 0)) {
        g_failures++;
        fprintf(stderr, "FAIL %s:%d (%s): len %zu, want %zu\n", __FILE__,
                line, what, actual_len, expected_len);
        for (size_t i = 0; i < actual_len && i < expected_len; i++) {
            if (actual[i] != expected[i]) {
                fprintf(stderr, "  first diff @%zu: %02X vs %02X\n", i,
                        actual[i], expected[i]);
                break;
            }
        }
    }
    g_checks++;
}

#define CHECK_BYTES(a, alen, e, elen, what) \
    check_bytes((a), (alen), (e), (elen), (what), __LINE__)

/* ------------------------------------------------------------------ *
 * Golden vectors (diagnostic notation in contract doc)
 * ------------------------------------------------------------------ */

/* {0:2, 1:"device_ack", 2:"ref_01", 3:"ping", 10:1, 4:0, 5:true}
 * Field order mirrors the Gateway QCBOR encoder; map order is not
 * contract but byte-equality eases capture diffing. */
static const uint8_t GOLDEN_ACK[] = {
    0xA7, 0x00, 0x02,                                     /* map(7), v=2 */
    0x01, 0x6A, 'd', 'e', 'v', 'i', 'c', 'e', '_', 'a', 'c', 'k',
    0x02, 0x66, 'r', 'e', 'f', '_', '0', '1',
    0x03, 0x64, 'p', 'i', 'n', 'g',
    0x0A, 0x01,                                           /* request 1  */
    0x04, 0x00,                                           /* int 0      */
    0x05, 0xF5,                                           /* true       */
};

/* {0:2, 1:"device_event", 2:"relay_01", 3:"button_pressed", 4:1, 5:true} */
static const uint8_t GOLDEN_EVENT[] = {
    0xA6, 0x00, 0x02,                                     /* map(6), v=2 */
    0x01, 0x6C, 'd', 'e', 'v', 'i', 'c', 'e', '_', 'e', 'v', 'e', 'n', 't',
    0x02, 0x68, 'r', 'e', 'l', 'a', 'y', '_', '0', '1',
    0x03, 0x6E, 'b', 'u', 't', 't', 'o', 'n', '_', 'p', 'r', 'e', 's', 's', 'e', 'd',
    0x04, 0x01,
    0x05, 0xF5,
};

/* ------------------------------------------------------------------ */

static void test_encode_golden_ack(void)
{
    gw_message_t request, ack;
    uint8_t buf[GW_MSG_MAX_LEN];

    gw_message_init(&request);
    request.protocol_version = 2;
    strcpy(request.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(request.device_id, "ref_01");
    request.has_device_id = 1;
    strcpy(request.command, "ping");
    request.request_id = 1;
    request.has_request_id = 1;

    gw_build_ack(&ack, &request, NULL, true, 0);
    CHECK(gw_message_valid_ack(&ack));
    int encoded = gw_message_encode(&ack, buf, sizeof(buf));
    CHECK_INT(encoded, (int)sizeof(GOLDEN_ACK));
    CHECK_BYTES(buf, (size_t)encoded, GOLDEN_ACK, sizeof(GOLDEN_ACK),
                "golden ack");

    /* Decode the golden vector back and verify every echoed field. */
    gw_message_t decoded;
    CHECK_INT(gw_message_decode(GOLDEN_ACK, sizeof(GOLDEN_ACK), &decoded),
              GW_OK);
    CHECK(strcmp(decoded.type, "device_ack") == 0);
    CHECK(strcmp(decoded.device_id, "ref_01") == 0);
    CHECK(decoded.has_device_id == 1);
    CHECK(strcmp(decoded.command, "ping") == 0);
    CHECK(decoded.int_value == 0);
    CHECK(decoded.bool_value == 1);
    CHECK(decoded.has_request_id == 1);
    CHECK(decoded.request_id == 1);
    CHECK(decoded.protocol_version == 2);
}

static void test_decode_golden_vectors(void)
{
    /* Contract #185: negative command vector must still decode so the
     * Device can answer with a failure ACK. */
    static const uint8_t NEGATIVE_CMD[] = {
        0xA7, 0x00, 0x02,
        0x01, 0x6E, 'd', 'e', 'v', 'i', 'c', 'e', '_', 'c', 'o', 'm', 'm', 'a', 'n', 'd',
        0x02, 0x66, 'r', 'e', 'f', '_', '0', '1',
        0x03, 0x6B, 'u', 'n', 'k', 'n', 'o', 'w', 'n', '_', 'x', 'y', 'z',
        0x04, 0x00,
        0x05, 0xF4,
        0x0A, 0x02,
    };
    gw_message_t decoded;
    CHECK_INT(gw_message_decode(NEGATIVE_CMD, sizeof(NEGATIVE_CMD), &decoded),
              GW_OK);
    CHECK(strcmp(decoded.command, "unknown_xyz") == 0);
    CHECK(strcmp(decoded.device_id, "ref_01") == 0);

    CHECK_INT(gw_message_decode(GOLDEN_EVENT, sizeof(GOLDEN_EVENT), &decoded),
              GW_OK);
    CHECK(strcmp(decoded.type, "device_event") == 0);
    CHECK(strcmp(decoded.command, "button_pressed") == 0);
    CHECK(strcmp(decoded.device_id, "relay_01") == 0);
    CHECK(decoded.int_value == 1);
    CHECK(decoded.bool_value == 1);
    CHECK(decoded.has_request_id == 0);
}

static void test_build_and_roundtrip(void)
{
    gw_message_t msg, decoded;
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded;

    /* ACK failure keeps exact echo semantics (#81/#82). */
    gw_message_t request;
    gw_message_init(&request);
    strcpy(request.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(request.device_id, "relay_01");
    request.has_device_id = 1;
    strcpy(request.command, "set_state");
    request.request_id = 142;
    request.has_request_id = 1;

    gw_build_ack(&msg, &request, NULL, false, 0);
    CHECK(gw_message_valid_ack(&msg));
    CHECK(msg.request_id == 142 && msg.has_request_id == 1);
    CHECK(strcmp(msg.command, "set_state") == 0);
    CHECK(msg.bool_value == 0);

    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK(gw_message_decode(buf, (size_t)encoded, &decoded) == GW_OK);
    CHECK(decoded.request_id == 142);
    CHECK(strcmp(decoded.command, "set_state") == 0);
    CHECK(decoded.bool_value == 0);

    /* Event round trip, bool false path (#107). */
    gw_build_event(&msg, "sensor_bedroom", "temperature_mC", 25375, false);
    CHECK(gw_message_valid_event(&msg));
    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK(gw_message_decode(buf, (size_t)encoded, &decoded) == GW_OK);
    CHECK(strcmp(decoded.type, GW_MSG_TYPE_DEVICE_EVENT) == 0);
    CHECK(strcmp(decoded.command, "temperature_mC") == 0);
    CHECK(strcmp(decoded.device_id, "sensor_bedroom") == 0);
    CHECK(decoded.int_value == 25375);
    CHECK(decoded.bool_value == 0);

    /* Negative int + int32 boundaries. */
    gw_build_event(&msg, "s", "neg", -25375, true);
    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK(gw_message_decode(buf, (size_t)encoded, &decoded) == GW_OK);
    CHECK(decoded.int_value == -25375);

    gw_build_event(&msg, "s", "max", INT32_MAX, true);
    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK(gw_message_decode(buf, (size_t)encoded, &decoded) == GW_OK);
    CHECK(decoded.int_value == INT32_MAX);

    gw_build_event(&msg, "s", "min", INT32_MIN, true);
    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK(gw_message_decode(buf, (size_t)encoded, &decoded) == GW_OK);
    CHECK(decoded.int_value == INT32_MIN);

    /* Optional metadata fields + extended-length string headers. */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_EVENT);
    strcpy(msg.device_id, "aaaaaaaaaaaaaaaaaaaaaaaa"); /* 24 chars */
    msg.has_device_id = 1;
    strcpy(msg.command, "state_changed");
    strcpy(msg.name, "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn"); /* 31 chars */
    strcpy(msg.device_type, "ttttttttttttttt");          /* 15 chars */
    const uint8_t addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    memcpy(msg.ble_addr, addr, sizeof(addr));
    msg.ble_addr_type = 1;
    msg.has_ble_addr = 1;

    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0 && (size_t)encoded <= GW_MSG_MAX_LEN);
    CHECK(gw_message_decode(buf, (size_t)encoded, &decoded) == GW_OK);
    CHECK(strcmp(decoded.name, msg.name) == 0);
    CHECK(strcmp(decoded.device_type, msg.device_type) == 0);
    CHECK(decoded.has_device_id == 1);
    CHECK(memcmp(decoded.ble_addr, addr, sizeof(addr)) == 0);
    CHECK(decoded.ble_addr_type == 1);
    CHECK(decoded.has_ble_addr == 1);
}

static void test_decode_rejections(void)
{
    gw_message_t decoded;

    /* Missing required fields (contract #55, #186). */
    static const uint8_t NO_TYPE[] = { 0xA4, 0x00, 0x02, 0x03, 0x61, 'c',
                                       0x04, 0x00, 0x05, 0xF4 };
    static const uint8_t NO_COMMAND[] = { 0xA4, 0x00, 0x02, 0x01, 0x61, 't',
                                          0x04, 0x00, 0x05, 0xF4 };
    static const uint8_t NO_INT[] = { 0xA4, 0x00, 0x02, 0x01, 0x61, 't',
                                      0x03, 0x61, 'c', 0x05, 0xF4 };
    static const uint8_t NO_BOOL[] = { 0xA4, 0x00, 0x02, 0x01, 0x61, 't',
                                       0x03, 0x61, 'c', 0x04, 0x00 };
    CHECK_INT(gw_message_decode(NO_TYPE, sizeof(NO_TYPE), &decoded),
              GW_ERR_DECODE);
    CHECK_INT(gw_message_decode(NO_COMMAND, sizeof(NO_COMMAND), &decoded),
              GW_ERR_DECODE);
    CHECK_INT(gw_message_decode(NO_INT, sizeof(NO_INT), &decoded),
              GW_ERR_DECODE);
    CHECK_INT(gw_message_decode(NO_BOOL, sizeof(NO_BOOL), &decoded),
              GW_ERR_DECODE);

    /* Version handling: 0 and >4 rejected, absent defaults v4,
     * v1 tolerated at codec level. */
    static const uint8_t VERSION_0[] = { 0xA5, 0x00, 0x00, 0x01, 0x61, 't',
                                         0x03, 0x61, 'c', 0x04, 0x00,
                                         0x05, 0xF4 };
    static const uint8_t VERSION_5[] = { 0xA5, 0x00, 0x05, 0x01, 0x61, 't',
                                         0x03, 0x61, 'c', 0x04, 0x00,
                                         0x05, 0xF4 };
    CHECK_INT(gw_message_decode(VERSION_0, sizeof(VERSION_0), &decoded),
              GW_ERR_UNSUPPORTED_VERSION);
    CHECK_INT(gw_message_decode(VERSION_5, sizeof(VERSION_5), &decoded),
              GW_ERR_UNSUPPORTED_VERSION);

    static const uint8_t VERSION_ABSENT[] = { 0xA4, 0x01, 0x61, 't', 0x03,
                                              0x61, 'c', 0x04, 0x00,
                                              0x05, 0xF4 };
    CHECK_INT(
        gw_message_decode(VERSION_ABSENT, sizeof(VERSION_ABSENT), &decoded),
        GW_OK);
    CHECK(decoded.protocol_version == GW_PROTOCOL_VERSION);

    static const uint8_t VERSION_1[] = { 0xA5, 0x00, 0x01, 0x01, 0x61, 't',
                                         0x03, 0x61, 'c', 0x04, 0x00,
                                         0x05, 0xF4 };
    CHECK_INT(gw_message_decode(VERSION_1, sizeof(VERSION_1), &decoded),
              GW_OK);
    CHECK(decoded.protocol_version == 1);

    /* request_id rules (#60): zero rejected, absent allowed. */
    static const uint8_t REQUEST_ZERO[] = { 0xA7, 0x00, 0x02, 0x01, 0x61,
                                            't',  0x03, 0x61, 'c', 0x04,
                                            0x00, 0x05, 0xF4, 0x0A, 0x00 };
    /* request_id = 2^32 via 8-byte argument -> exceeds uint32 range. */
    static const uint8_t REQUEST_TOO_BIG[] = {
        0xA7, 0x00, 0x02, 0x01, 0x61, 't',    0x03, 0x61, 'c',
        0x04, 0x00, 0x05, 0xF4, 0x0A, 0x1B, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00,
    };
    CHECK_INT(gw_message_decode(REQUEST_ZERO, sizeof(REQUEST_ZERO), &decoded),
              GW_ERR_DECODE);
    CHECK_INT(
        gw_message_decode(REQUEST_TOO_BIG, sizeof(REQUEST_TOO_BIG), &decoded),
        GW_ERR_DECODE);

    /* Structural strictness. */
    static const uint8_t TRAILING_GARBAGE[] = {
        0xA7, 0x00, 0x02, 0x01, 0x6A, 'd', 'e', 'v', 'i', 'c', 'e', '_',
        'a',  'c',  'k',  0x02, 0x66, 'r', 'e', 'f', '_', '0', '1', 0x03,
        0x64, 'p',  'i',  'n',  'g',  0x0A, 0x01, 0x04, 0x00, 0x05, 0xF5,
        0xFF,
    };
    CHECK_INT(
        gw_message_decode(TRAILING_GARBAGE, sizeof(TRAILING_GARBAGE) - 1,
                          &decoded),
        GW_OK); /* sanity: without last byte it is the golden ACK */
    CHECK_INT(gw_message_decode(TRAILING_GARBAGE, sizeof(TRAILING_GARBAGE),
                                &decoded),
              GW_ERR_DECODE);

    static const uint8_t NOT_A_MAP[] = { 0x00 }; /* unsigned int */
    static const uint8_t INDEFINITE_MAP[] = { 0xBF, 0xFF }; /* map(...) inf */
    CHECK_INT(gw_message_decode(NOT_A_MAP, sizeof(NOT_A_MAP), &decoded),
              GW_ERR_DECODE);
    CHECK_INT(gw_message_decode(INDEFINITE_MAP, sizeof(INDEFINITE_MAP),
                                &decoded),
              GW_ERR_DECODE);

    /* Unknown keys are tolerated (mirror Gateway targeted lookups). */
    static const uint8_t UNKNOWN_KEYS[] = {
        0xA7, 0x00, 0x02, 0x01, 0x61, 't',  0x03, 0x61, 'c',
        0x04, 0x00, 0x05, 0xF4, 0x16, 0x62, 'z', 'z', 0x18,
        0x63, 0x80, /* key 99 -> empty array */
    };
    CHECK_INT(gw_message_decode(UNKNOWN_KEYS, sizeof(UNKNOWN_KEYS), &decoded),
              GW_OK);

    /* ble_addr without ble_addr_type is rejected (mirror Gateway). */
    static const uint8_t ADDR_NO_TYPE[] = {
        0xA6, 0x00, 0x02, 0x01, 0x61,  't',  0x03, 0x61, 'c',
        0x04, 0x00, 0x05, 0xF4, 0x08, 0x46, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06,
    };
    CHECK_INT(gw_message_decode(ADDR_NO_TYPE, sizeof(ADDR_NO_TYPE), &decoded),
              GW_ERR_DECODE);

    static const uint8_t ADDR_WITH_TYPE[] = {
        0xA7, 0x00, 0x02, 0x01, 0x61,  't',  0x03, 0x61, 'c',
        0x04, 0x00, 0x05, 0xF4, 0x08, 0x46, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x09, 0x01,
    };
    CHECK_INT(
        gw_message_decode(ADDR_WITH_TYPE, sizeof(ADDR_WITH_TYPE), &decoded),
        GW_OK);
    CHECK(decoded.has_ble_addr == 1 && decoded.ble_addr_type == 1);

    /* Size guards (#45/#188). */
    static uint8_t oversized[GW_MSG_MAX_LEN + 1];
    memset(oversized, 0xA1, sizeof(oversized)); /* one-pair maps, junk */
    CHECK_INT(gw_message_decode(oversized, sizeof(oversized), &decoded),
              GW_ERR_INVALID_ARG);
    CHECK_INT(gw_message_decode(oversized, 0, &decoded), GW_ERR_INVALID_ARG);
    CHECK_INT(gw_message_decode(NULL, 8, &decoded), GW_ERR_INVALID_ARG);
}

static void test_capability_v3_roundtrip(void)
{
    gw_message_t item, decoded;
    uint8_t buf[GW_MSG_MAX_LEN];
    gw_message_init(&item);
    strcpy(item.type, GW_MSG_TYPE_CAPABILITY_ITEM);
    strcpy(item.device_id, "lamp-01");
    item.has_device_id = 1;
    strcpy(item.command, "set_brightness");
    item.snapshot_id = 88;
    item.has_snapshot_id = 1;
    item.sequence = 1;
    item.has_sequence = 1;
    item.value_type = 2;
    item.has_value_type = 1;
    item.capability_flags = 1;
    item.has_capability_flags = 1;
    item.min_value = 0;
    item.has_min_value = 1;
    item.max_value = 100;
    item.has_max_value = 1;
    item.step = 5;
    item.has_step = 1;
    strcpy(item.capability_label, "Brightness");
    strcpy(item.capability_unit, "%");

    int encoded = gw_message_encode(&item, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK_INT(decoded.protocol_version, 4);
    CHECK(strcmp(decoded.type, GW_MSG_TYPE_CAPABILITY_ITEM) == 0);
    CHECK_INT(decoded.snapshot_id, 88);
    CHECK(decoded.has_snapshot_id);
    CHECK_INT(decoded.sequence, 1);
    CHECK_INT(decoded.value_type, 2);
    CHECK_INT(decoded.capability_flags, 1);
    CHECK_INT(decoded.min_value, 0);
    CHECK_INT(decoded.max_value, 100);
    CHECK_INT(decoded.step, 5);
    CHECK(strcmp(decoded.capability_label, "Brightness") == 0);
    CHECK(strcmp(decoded.capability_unit, "%") == 0);
}

static void test_encode_validation(void)
{
    gw_message_t msg;
    uint8_t small[10];
    uint8_t buf[GW_MSG_MAX_LEN];

    gw_message_init(&msg);
    CHECK_INT(gw_message_encode(NULL, buf, sizeof(buf)), GW_ERR_INVALID_ARG);
    CHECK_INT(gw_message_encode(&msg, NULL, sizeof(buf)), GW_ERR_INVALID_ARG);
    CHECK_INT(gw_message_encode(&msg, buf, 0), GW_ERR_INVALID_ARG);

    /* Empty type/command never reach the wire. */
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_EVENT); /* but empty command */
    CHECK_INT(gw_message_encode(&msg, buf, sizeof(buf)), GW_ERR_INVALID_ARG);

    /* Version > current protocol rejected (#50). */
    msg.command[0] = 'x';
    msg.protocol_version = GW_PROTOCOL_VERSION + 1;
    CHECK_INT(gw_message_encode(&msg, buf, sizeof(buf)), GW_ERR_VALIDATION);

    /* request_id == 0 with presence flag is not wire-valid (#60). */
    msg.protocol_version = 0; /* defaults to current protocol */
    msg.has_request_id = 1;
    msg.request_id = 0;
    CHECK_INT(gw_message_encode(&msg, buf, sizeof(buf)), GW_ERR_VALIDATION);

    /* Claimed device_id must actually carry an identity (#63). */
    msg.has_request_id = 0;
    msg.has_device_id = 1; /* device_id still empty */
    CHECK_INT(gw_message_encode(&msg, buf, sizeof(buf)), GW_ERR_VALIDATION);

    /* Buffer smaller than encoded output -> clean failure, no overflow. */
    msg.has_device_id = 0;
    int needed = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(needed > 0);
    CHECK_INT(gw_message_encode(&msg, small, sizeof(small)), GW_ERR_NO_SPACE);
}

static void test_string_limits(void)
{
    gw_message_t msg, decoded;
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded;

    /* Contract #54/#187: effective max = LEN-1. Boundary values pass. */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_ACK);
    strcpy(msg.device_id, "ddddddddddddddddddddddddddddddd"); /* 31 */
    msg.has_device_id = 1;
    strcpy(msg.command, "ccccccccccccccccccccccccccccccc");   /* 31 */
    strcpy(msg.device_type, "ttttttttttttttt");               /* 15 */
    msg.has_request_id = 1;
    msg.request_id = 4294967295u; /* UINT32_MAX */

    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK(gw_message_decode(buf, (size_t)encoded, &decoded) == GW_OK);
    CHECK(strlen(decoded.device_id) == 31);
    CHECK(strlen(decoded.command) == 31);
    CHECK(strlen(decoded.device_type) == 15);
    CHECK(decoded.request_id == 4294967295u);

    /* Over-capacity strings are caught before encoding. */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_ACK);
    memset(msg.command, 'C', sizeof(msg.command)); /* 32 chars, no NUL */
    CHECK_INT(gw_message_encode(&msg, buf, sizeof(buf)), GW_ERR_INVALID_ARG);

    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_ACK);
    strcpy(msg.command, "ok");
    memset(msg.device_type, 'T', sizeof(msg.device_type)); /* 16, no NUL */
    CHECK_INT(gw_message_encode(&msg, buf, sizeof(buf)), GW_ERR_VALIDATION);
}

static void test_feature_v4_roundtrip(void)
{
    gw_message_t msg, decoded;
    uint8_t buf[GW_MSG_MAX_LEN];

    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_FEATURE_ITEM);
    strcpy(msg.command, GW_COMMAND_DESCRIBE_CAPABILITIES);
    strcpy(msg.feature_id, "led_main");
    msg.has_feature_id = 1;
    msg.feature_type = GW_FEATURE_ON_OFF_LIGHT;
    msg.has_feature_type = 1;
    msg.feature_schema_version = 1;
    msg.has_feature_schema_version = 1;
    msg.property_id = GW_PROP_ON_OFF;
    msg.has_property_id = 1;
    strcpy(msg.feature_tool, "set_led");
    msg.has_feature_tool = 1;
    msg.value_type = 1;
    msg.has_value_type = 1;
    int encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK(gw_message_decode(buf, (size_t)encoded, &decoded) == GW_OK);
    CHECK(decoded.protocol_version == 4);
    CHECK(decoded.has_feature_id && strcmp(decoded.feature_id, "led_main") == 0);
    CHECK(decoded.feature_type == GW_FEATURE_ON_OFF_LIGHT);
    CHECK(decoded.feature_schema_version == 1);
    CHECK(decoded.property_id == GW_PROP_ON_OFF);
    CHECK(decoded.has_feature_tool && strcmp(decoded.feature_tool, "set_led") == 0);

    gw_build_feature_event_bool(&msg, "esp32s3-ref", "led_main",
                                GW_PROP_ON_OFF, true);
    encoded = gw_message_encode(&msg, buf, sizeof(buf));
    CHECK(encoded > 0);
    CHECK(gw_message_decode(buf, (size_t)encoded, &decoded) == GW_OK);
    CHECK(strcmp(decoded.command, GW_EVENT_FEATURE_STATE) == 0);
    CHECK(decoded.has_feature_value_bool && decoded.feature_value_bool);
    CHECK(decoded.has_feature_id && decoded.has_property_id);
}

int main(void)
{
    test_encode_golden_ack();
    test_decode_golden_vectors();
    test_build_and_roundtrip();
    test_decode_rejections();
    test_encode_validation();
    test_string_limits();
    test_capability_v3_roundtrip();
    test_feature_v4_roundtrip();

    printf("gateway_protocol: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
