/*
 * Host unit tests for components/gateway_settings (Settings v2 codec).
 *
 * Run: test/host/run_gateway_settings_tests.sh
 */
#include <stdio.h>
#include <string.h>

#include "gateway_protocol.h"
#include "gateway_settings.h"

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
 * Settings encode tests
 * ------------------------------------------------------------------ */

static void test_settings_encode_begin(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_settings_encode_begin(buf, sizeof(buf), 5, 42);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    /* Decode and verify */
    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK_INT(decoded.protocol_version, GW_PROTOCOL_VERSION);
    CHECK(decoded.has_total == 1);
    CHECK_INT(decoded.total, 5);
    CHECK(decoded.has_request_id == 1);
    CHECK_INT(decoded.request_id, 42);
}

static void test_settings_encode_item(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_settings_encode_item(buf, sizeof(buf),
                                          0, 3, 10,
                                          "wifi_ssid", "WiFi SSID", "network", "",
                                          GW_SETTING_TYPE_STRING,
                                          0, 32,
                                          0, 0, 0);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_setting_id == 1);
    CHECK(strcmp(decoded.setting_id, "wifi_ssid") == 0);
    CHECK(strcmp(decoded.setting_title, "WiFi SSID") == 0);
    CHECK(strcmp(decoded.setting_group, "network") == 0);
    CHECK(decoded.setting_type == GW_SETTING_TYPE_STRING);
    CHECK(decoded.setting_max_length == 32);
}

static void test_settings_encode_item_int(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_settings_encode_item(buf, sizeof(buf),
                                          1, 3, 11,
                                          "sample_rate", "Sample Rate", "", "Hz",
                                          GW_SETTING_TYPE_INT,
                                          GW_SETTING_FLAG_ADVANCED, 0,
                                          1, 100, 1);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_setting_id == 1);
    CHECK(strcmp(decoded.setting_id, "sample_rate") == 0);
    CHECK(decoded.setting_type == GW_SETTING_TYPE_INT);
    CHECK(decoded.setting_flags == GW_SETTING_FLAG_ADVANCED);
    CHECK(decoded.setting_max_length == 0);
}

static void test_settings_encode_option_item(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_settings_encode_option_item(buf, sizeof(buf),
                                                 2, 0, 12,
                                                 "Low Power");

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.setting_option_index == 0);
    CHECK(strcmp(decoded.setting_title, "Low Power") == 0);
}

static void test_settings_encode_end(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_settings_encode_end(buf, sizeof(buf), 3, 13);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_total == 1);
    CHECK_INT(decoded.total, 3);
}

static void test_settings_encode_values_begin(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_settings_encode_values_begin(buf, sizeof(buf), 5, 42, 20);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_total == 1);
    CHECK_INT(decoded.total, 5);
    CHECK(decoded.has_capability_revision == 1);
    CHECK_INT(decoded.capability_revision, 42);
    CHECK(decoded.has_request_id == 1);
    CHECK_INT(decoded.request_id, 20);
}

static void test_settings_encode_value_bool(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    bool value = true;
    int encoded = gw_settings_encode_value(buf, sizeof(buf),
                                           0, "enabled",
                                           GW_SETTING_TYPE_BOOL,
                                           &value, 21);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_setting_id == 1);
    CHECK(strcmp(decoded.setting_id, "enabled") == 0);
    CHECK(decoded.setting_type == GW_SETTING_TYPE_BOOL);
}

static void test_settings_encode_value_int(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int32_t value = 42;
    int encoded = gw_settings_encode_value(buf, sizeof(buf),
                                           1, "count",
                                           GW_SETTING_TYPE_INT,
                                           &value, 22);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_setting_id == 1);
    CHECK(strcmp(decoded.setting_id, "count") == 0);
    CHECK(decoded.setting_type == GW_SETTING_TYPE_INT);
}

static void test_settings_encode_value_string(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    const char *value = "hello";
    int encoded = gw_settings_encode_value(buf, sizeof(buf),
                                           2, "name",
                                           GW_SETTING_TYPE_STRING,
                                           value, 23);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_setting_id == 1);
    CHECK(strcmp(decoded.setting_id, "name") == 0);
    CHECK(decoded.setting_type == GW_SETTING_TYPE_STRING);
}

static void test_settings_encode_value_enum(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    uint8_t value = 2;
    int encoded = gw_settings_encode_value(buf, sizeof(buf),
                                           3, "mode",
                                           GW_SETTING_TYPE_ENUM,
                                           &value, 24);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_setting_id == 1);
    CHECK(strcmp(decoded.setting_id, "mode") == 0);
    CHECK(decoded.setting_type == GW_SETTING_TYPE_ENUM);
}

static void test_settings_encode_values_end(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_settings_encode_values_end(buf, sizeof(buf), 5, 42, 25);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_total == 1);
    CHECK_INT(decoded.total, 5);
    CHECK(decoded.has_capability_revision == 1);
    CHECK_INT(decoded.capability_revision, 42);
}

/* ------------------------------------------------------------------ *
 * Transaction encode tests
 * ------------------------------------------------------------------ */

static void test_settings_encode_tx_begin(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_settings_encode_tx_begin(buf, sizeof(buf),
                                              0x1234567890ABCDEFULL, 1, 30);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_settings_transaction_id == 1);
    CHECK(decoded.settings_transaction_id == 0x1234567890ABCDEFULL);
    CHECK(decoded.has_settings_expected_revision == 1);
    CHECK_INT(decoded.settings_expected_revision, 1);
}

static void test_settings_encode_tx_set(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int32_t value = 100;
    int encoded = gw_settings_encode_tx_set(buf, sizeof(buf),
                                            0x1234567890ABCDEFULL,
                                            "sample_rate",
                                            GW_SETTING_TYPE_INT,
                                            &value, 31);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_settings_transaction_id == 1);
    CHECK(decoded.settings_transaction_id == 0x1234567890ABCDEFULL);
    CHECK(decoded.has_setting_id == 1);
    CHECK(strcmp(decoded.setting_id, "sample_rate") == 0);
    CHECK(decoded.setting_type == GW_SETTING_TYPE_INT);
}

static void test_settings_encode_tx_commit(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_settings_encode_tx_commit(buf, sizeof(buf),
                                               0x1234567890ABCDEFULL, 32);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_settings_transaction_id == 1);
    CHECK(decoded.settings_transaction_id == 0x1234567890ABCDEFULL);
}

static void test_settings_encode_tx_abort(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_settings_encode_tx_abort(buf, sizeof(buf),
                                              0x1234567890ABCDEFULL, 33);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_settings_transaction_id == 1);
    CHECK(decoded.settings_transaction_id == 0x1234567890ABCDEFULL);
}

static void test_settings_encode_commit_confirm(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];
    int encoded = gw_settings_encode_commit_confirm(buf, sizeof(buf),
                                                    0x1234567890ABCDEFULL,
                                                    42, 34);

    CHECK(encoded > 0);
    CHECK((size_t)encoded <= GW_MSG_MAX_LEN);

    gw_message_t decoded;
    CHECK_INT(gw_message_decode(buf, (size_t)encoded, &decoded), GW_OK);
    CHECK(decoded.has_settings_transaction_id == 1);
    CHECK(decoded.settings_transaction_id == 0x1234567890ABCDEFULL);
    CHECK(decoded.has_settings_new_revision == 1);
    CHECK_INT(decoded.settings_new_revision, 42);
}

/* ------------------------------------------------------------------ *
 * Decode tests
 * ------------------------------------------------------------------ */

static void test_settings_decode_command(void)
{
    gw_message_t msg;
    gw_settings_command_t cmd;

    /* Test describe_settings */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(msg.command, GW_COMMAND_DESCRIBE_SETTINGS);
    msg.request_id = 100;
    msg.has_request_id = 1;

    CHECK_INT(gw_settings_decode_command(&msg, &cmd), GW_OK);
    CHECK(cmd.cmd_type == GW_SETTINGS_CMD_DESCRIBE);
    CHECK(cmd.has_request_id == 1);
    CHECK_INT(cmd.request_id, 100);

    /* Test read_settings */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(msg.command, GW_COMMAND_READ_SETTINGS);
    msg.request_id = 101;
    msg.has_request_id = 1;

    CHECK_INT(gw_settings_decode_command(&msg, &cmd), GW_OK);
    CHECK(cmd.cmd_type == GW_SETTINGS_CMD_READ);

    /* Test settings_tx_begin */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(msg.command, GW_MSG_TYPE_SETTINGS_TX_BEGIN);
    msg.request_id = 102;
    msg.has_request_id = 1;
    msg.settings_transaction_id = 0xDEADBEEF;
    msg.has_settings_transaction_id = 1;
    msg.settings_expected_revision = 5;
    msg.has_settings_expected_revision = 1;

    CHECK_INT(gw_settings_decode_command(&msg, &cmd), GW_OK);
    CHECK(cmd.cmd_type == GW_SETTINGS_CMD_TX_BEGIN);
    CHECK(cmd.has_transaction_id == 1);
    CHECK(cmd.transaction_id == 0xDEADBEEF);
    CHECK(cmd.has_expected_revision == 1);
    CHECK_INT(cmd.expected_revision, 5);

    /* Test settings_tx_set */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(msg.command, GW_MSG_TYPE_SETTINGS_TX_SET);
    msg.request_id = 103;
    msg.has_request_id = 1;
    msg.settings_transaction_id = 0xCAFEBABE;
    msg.has_settings_transaction_id = 1;
    strcpy(msg.setting_id, "wifi_ssid");
    msg.has_setting_id = 1;
    msg.setting_type = GW_SETTING_TYPE_STRING;
    msg.has_setting_type = 1;

    CHECK_INT(gw_settings_decode_command(&msg, &cmd), GW_OK);
    CHECK(cmd.cmd_type == GW_SETTINGS_CMD_TX_SET);
    CHECK(cmd.has_transaction_id == 1);
    CHECK(cmd.transaction_id == 0xCAFEBABE);
    CHECK(cmd.has_setting_id == 1);
    CHECK(strcmp(cmd.setting_id, "wifi_ssid") == 0);

    /* Test settings_tx_commit */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(msg.command, GW_MSG_TYPE_SETTINGS_TX_COMMIT);
    msg.request_id = 104;
    msg.has_request_id = 1;
    msg.settings_transaction_id = 0x12345678;
    msg.has_settings_transaction_id = 1;

    CHECK_INT(gw_settings_decode_command(&msg, &cmd), GW_OK);
    CHECK(cmd.cmd_type == GW_SETTINGS_CMD_TX_COMMIT);

    /* Test settings_tx_abort */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(msg.command, GW_MSG_TYPE_SETTINGS_TX_ABORT);
    msg.request_id = 105;
    msg.has_request_id = 1;
    msg.settings_transaction_id = 0x87654321;
    msg.has_settings_transaction_id = 1;

    CHECK_INT(gw_settings_decode_command(&msg, &cmd), GW_OK);
    CHECK(cmd.cmd_type == GW_SETTINGS_CMD_TX_ABORT);

    /* Test unknown command */
    gw_message_init(&msg);
    strcpy(msg.type, GW_MSG_TYPE_DEVICE_COMMAND);
    strcpy(msg.command, "unknown_command");

    CHECK_INT(gw_settings_decode_command(&msg, &cmd), GW_ERR_DECODE);
}

/* ------------------------------------------------------------------ *
 * Validation tests
 * ------------------------------------------------------------------ */

static void test_settings_encode_validation(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];

    /* NULL buffer */
    CHECK_INT(gw_settings_encode_begin(NULL, 100, 5, 1), GW_ERR_INVALID_ARG);
    CHECK_INT(gw_settings_encode_begin(buf, 0, 5, 1), GW_ERR_INVALID_ARG);

    /* NULL setting_id */
    CHECK_INT(gw_settings_encode_item(buf, sizeof(buf),
                                      0, 3, 1,
                                      NULL, "Title", "", "",
                                      GW_SETTING_TYPE_BOOL, 0, 0,
                                      0, 0, 0),
              GW_ERR_INVALID_ARG);

    /* NULL title */
    CHECK_INT(gw_settings_encode_item(buf, sizeof(buf),
                                      0, 3, 1,
                                      "id", NULL, "", "",
                                      GW_SETTING_TYPE_BOOL, 0, 0,
                                      0, 0, 0),
              GW_ERR_INVALID_ARG);

    /* NULL option_label */
    CHECK_INT(gw_settings_encode_option_item(buf, sizeof(buf),
                                             0, 0, 1,
                                             NULL),
              GW_ERR_INVALID_ARG);

    /* NULL value */
    CHECK_INT(gw_settings_encode_value(buf, sizeof(buf),
                                       0, "id", GW_SETTING_TYPE_BOOL,
                                       NULL, 1),
              GW_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------------ *
 * Budget test — ensure all settings messages fit in GW_MSG_MAX_LEN
 * ------------------------------------------------------------------ */

static void test_settings_message_budget(void)
{
    uint8_t buf[GW_MSG_MAX_LEN];

    /* All message types must fit within GW_MSG_MAX_LEN */
    CHECK(gw_settings_encode_begin(buf, sizeof(buf), 12, 1) > 0);
    CHECK(gw_settings_encode_item(buf, sizeof(buf), 0, 12, 1,
                                  "setting_id_with_long_name", "Setting Title With Long Name",
                                  "group_name", "unit",
                                  GW_SETTING_TYPE_INT,
                                  GW_SETTING_FLAG_READONLY | GW_SETTING_FLAG_SECRET,
                                  64, -1000, 1000, 10) > 0);
    CHECK(gw_settings_encode_option_item(buf, sizeof(buf), 0, 7, 1,
                                         "Option Label Long") > 0);
    CHECK(gw_settings_encode_end(buf, sizeof(buf), 12, 1) > 0);
    CHECK(gw_settings_encode_values_begin(buf, sizeof(buf), 12, UINT32_MAX, 1) > 0);
    CHECK(gw_settings_encode_value(buf, sizeof(buf), 0, "id",
                                   GW_SETTING_TYPE_STRING,
                                   "long_string_value_that_is_relatively_long_but_still_fits", 1) > 0);
    CHECK(gw_settings_encode_values_end(buf, sizeof(buf), 12, UINT32_MAX, 1) > 0);
    CHECK(gw_settings_encode_tx_begin(buf, sizeof(buf), UINT64_MAX, UINT32_MAX, 1) > 0);
    CHECK(gw_settings_encode_tx_set(buf, sizeof(buf), UINT64_MAX, "id",
                                    GW_SETTING_TYPE_STRING,
                                    "value", 1) > 0);
    CHECK(gw_settings_encode_tx_commit(buf, sizeof(buf), UINT64_MAX, 1) > 0);
    CHECK(gw_settings_encode_tx_abort(buf, sizeof(buf), UINT64_MAX, 1) > 0);
    CHECK(gw_settings_encode_commit_confirm(buf, sizeof(buf), UINT64_MAX, UINT32_MAX, 1) > 0);
}

/* ------------------------------------------------------------------ *
 * Settings fields decode in gw_message_decode
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Main
 * ------------------------------------------------------------------ */

int main(void)
{
    test_settings_encode_begin();
    test_settings_encode_item();
    test_settings_encode_item_int();
    test_settings_encode_option_item();
    test_settings_encode_end();
    test_settings_encode_values_begin();
    test_settings_encode_value_bool();
    test_settings_encode_value_int();
    test_settings_encode_value_string();
    test_settings_encode_value_enum();
    test_settings_encode_values_end();
    test_settings_encode_tx_begin();
    test_settings_encode_tx_set();
    test_settings_encode_tx_commit();
    test_settings_encode_tx_abort();
    test_settings_encode_commit_confirm();
    test_settings_decode_command();
    test_settings_encode_validation();
    test_settings_message_budget();

    printf("gateway_settings: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}