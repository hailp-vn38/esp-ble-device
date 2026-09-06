#include "demo_product.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "demo_board.h"
#include "demo_gpio_output.h"
#include "demo_input.h"
#include "demo_pwm.h"
#include "demo_sensor.h"
#include "device_command.h"
#include "device_event.h"
#include "device_feature.h"
#include "esp_log.h"

static const char *TAG = "demo_product";

static bool s_relay, s_plug, s_light, s_contact;
static int32_t s_dimmer, s_fan, s_temperature = 250, s_humidity = 55;
static int32_t s_dryer_temperature = 300, s_drying_time = 1;

static void publish_bool(const char *id, uint8_t property, bool value)
{
    if (device_feature_publish_bool(id, property, value) != 0)
        ESP_LOGW(TAG, "publish bool failed: %s", id);
}

static void publish_int(const char *id, uint8_t property, int32_t value)
{
    if (device_feature_publish_int(id, property, value) != 0)
        ESP_LOGW(TAG, "publish int failed: %s", id);
}

static device_cmd_result_t set_bool(const gw_message_t *request,
                                    device_cmd_response_t *response,
                                    bool *state, const char *id, uint8_t property,
                                    int (*apply)(bool))
{
    bool value = request->bool_value != 0;
    if (apply(value) != 0) { response->success = false; return DEVICE_CMD_ERR_HANDLER; }
    bool changed = *state != value;
    *state = value;
    if (changed) publish_bool(id, property, value);
    response->success = true;
    response->int_value = value ? 1 : 0;
    device_command_response_set_feature_bool(response, id, property, value);
    return DEVICE_CMD_OK;
}

static int apply_relay(bool value) { return demo_gpio_output_set(DEMO_RELAY_GPIO, value); }
static int apply_plug(bool value) { return demo_gpio_output_set(DEMO_PLUG_GPIO, value); }
static int apply_light(bool value) { return demo_gpio_output_set(DEMO_LIGHT_GPIO, value); }

static device_cmd_result_t cmd_relay(const gw_message_t *r, device_cmd_response_t *o) { return set_bool(r,o,&s_relay,"relay_main",GW_PROP_ON_OFF,apply_relay); }
static device_cmd_result_t cmd_plug(const gw_message_t *r, device_cmd_response_t *o) { return set_bool(r,o,&s_plug,"plug_main",GW_PROP_ON_OFF,apply_plug); }
static device_cmd_result_t cmd_light(const gw_message_t *r, device_cmd_response_t *o) { return set_bool(r,o,&s_light,"light_main",GW_PROP_ON_OFF,apply_light); }

static device_cmd_result_t set_pwm(const gw_message_t *request,
                                   device_cmd_response_t *response,
                                   int32_t *state, const char *id, uint8_t property,
                                   gpio_num_t pin, int channel)
{
    int32_t value = request->int_value;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    if (demo_pwm_set(pin, channel, value) != 0) { response->success = false; return DEVICE_CMD_ERR_HANDLER; }
    *state = value;
    publish_int(id, property, value);
    response->success = true; response->int_value = value;
    device_command_response_set_feature_int(response, id, property, value);
    return DEVICE_CMD_OK;
}

static device_cmd_result_t cmd_dimmer(const gw_message_t *r, device_cmd_response_t *o) { return set_pwm(r,o,&s_dimmer,"dimmer_main",GW_PROP_LEVEL,DEMO_DIMMER_PWM_GPIO,0); }
static device_cmd_result_t cmd_fan(const gw_message_t *r, device_cmd_response_t *o) { return set_pwm(r,o,&s_fan,"fan_main",GW_PROP_PERCENT_SETTING,DEMO_FAN_PWM_GPIO,1); }

static device_cmd_result_t set_value(const gw_message_t *request,
                                     device_cmd_response_t *response,
                                     int32_t *state, int32_t minimum, int32_t maximum,
                                     const char *id)
{
    int32_t value = request->int_value;
    if (value < minimum || value > maximum) { response->success = false; return DEVICE_CMD_ERR_VALIDATION; }
    *state = value; response->success = true; response->int_value = value;
    device_command_response_set_feature_int(response, id, GW_PROP_VALUE, value);
    publish_int(id, GW_PROP_VALUE, value);
    return DEVICE_CMD_OK;
}
static device_cmd_result_t cmd_dryer_temp(const gw_message_t *r, device_cmd_response_t *o) { return set_value(r,o,&s_dryer_temperature,300,1000,"dryer_temperature"); }
static device_cmd_result_t cmd_drying_time(const gw_message_t *r, device_cmd_response_t *o) { return set_value(r,o,&s_drying_time,1,180,"drying_time"); }

static int read_bool(void *context, bool *out) { *out = *(bool *)context; return 0; }
static int read_int(void *context, int32_t *out) { *out = *(int32_t *)context; return 0; }

static void sensor_sample(const demo_environment_sample_t *sample, void *context)
{
    (void)context;
    if (sample->temperature_raw != s_temperature) { s_temperature = sample->temperature_raw; publish_int("temperature_main", GW_PROP_TEMPERATURE, s_temperature); }
    if (sample->humidity_raw != s_humidity) { s_humidity = sample->humidity_raw; publish_int("humidity_main", GW_PROP_HUMIDITY, s_humidity); }
    bool contact = gpio_get_level(DEMO_CONTACT_GPIO) == 0;
    if (contact != s_contact) { s_contact = contact; publish_bool("contact_main", GW_PROP_CONTACT, s_contact); }
}

static void local_button(void *context)
{
    (void)context;
    s_relay = !s_relay;
    apply_relay(s_relay);
    publish_bool("relay_main", GW_PROP_ON_OFF, s_relay);
    ESP_LOGI(TAG, "local button relay -> %s", s_relay ? "ON" : "OFF");
}

static int register_features(void)
{
#define BOOL_FEATURE(ID,TITLE,TYPE,PROP,STATE,TOOL) \
    do { const device_feature_config_t c = {.feature_id=ID,.title=TITLE,.unit="",.type=TYPE,.schema_version=1,.property_id=PROP,.value_type=DEVICE_FEATURE_VALUE_BOOL,.write_tool=TOOL,.reader={.read_bool=read_bool},.context=&STATE}; if (device_feature_register(&c) != 0) return -1; } while (0)
#define INT_FEATURE(ID,TITLE,UNIT,TYPE,PROP,DEC,STATE,TOOL) \
    do { const device_feature_config_t c = {.feature_id=ID,.title=TITLE,.unit=UNIT,.type=TYPE,.schema_version=1,.property_id=PROP,.value_type=DEVICE_FEATURE_VALUE_INT,.decimals=DEC,.write_tool=TOOL,.reader={.read_int=read_int},.context=&STATE}; if (device_feature_register(&c) != 0) return -1; } while (0)
    BOOL_FEATURE("relay_main", "Relay", GW_FEATURE_GENERIC_RELAY, GW_PROP_ON_OFF, s_relay, "set_relay");
    BOOL_FEATURE("plug_main", "Plug", GW_FEATURE_ON_OFF_PLUGIN_UNIT, GW_PROP_ON_OFF, s_plug, "set_plug");
    BOOL_FEATURE("light_main", "Light", GW_FEATURE_ON_OFF_LIGHT, GW_PROP_ON_OFF, s_light, "set_light");
    INT_FEATURE("dimmer_main", "Dimmer", "%", GW_FEATURE_DIMMABLE_LIGHT, GW_PROP_LEVEL, 0, s_dimmer, "set_brightness");
    INT_FEATURE("fan_main", "Fan", "%", GW_FEATURE_FAN, GW_PROP_PERCENT_SETTING, 0, s_fan, "set_fan_speed");
    INT_FEATURE("temperature_main", "Temperature", "°C", GW_FEATURE_TEMPERATURE_SENSOR, GW_PROP_TEMPERATURE, 1, s_temperature, NULL);
    INT_FEATURE("humidity_main", "Humidity", "%", GW_FEATURE_HUMIDITY_SENSOR, GW_PROP_HUMIDITY, 0, s_humidity, NULL);
    BOOL_FEATURE("contact_main", "Contact", GW_FEATURE_CONTACT_SENSOR, GW_PROP_CONTACT, s_contact, NULL);
    INT_FEATURE("dryer_temperature", "Nhiệt độ sấy", "°C", GW_FEATURE_GENERIC_VALUE, GW_PROP_VALUE, 1, s_dryer_temperature, "set_dryer_temperature");
    INT_FEATURE("drying_time", "Thời gian sấy", "min", GW_FEATURE_GENERIC_VALUE, GW_PROP_VALUE, 0, s_drying_time, "set_drying_time");
#undef BOOL_FEATURE
#undef INT_FEATURE
    return 0;
}

static int register_commands(void)
{
    const device_cmd_capability_t caps[] = {
        {"set_relay", "Relay", "", DEVICE_CMD_VALUE_BOOL, DEVICE_CMD_FLAG_IDEMPOTENT, 0, 0, 0},
        {"set_plug", "Plug", "", DEVICE_CMD_VALUE_BOOL, DEVICE_CMD_FLAG_IDEMPOTENT, 0, 0, 0},
        {"set_light", "Light", "", DEVICE_CMD_VALUE_BOOL, DEVICE_CMD_FLAG_IDEMPOTENT, 0, 0, 0},
        {"set_brightness", "Brightness", "%", DEVICE_CMD_VALUE_INT, DEVICE_CMD_FLAG_IDEMPOTENT, 0, 100, 1},
        {"set_fan_speed", "Fan speed", "%", DEVICE_CMD_VALUE_INT, DEVICE_CMD_FLAG_IDEMPOTENT, 0, 100, 1},
        {"set_dryer_temperature", "Nhiệt độ sấy", "°C", DEVICE_CMD_VALUE_INT, DEVICE_CMD_FLAG_IDEMPOTENT, 300, 1000, 5},
        {"set_drying_time", "Thời gian sấy", "min", DEVICE_CMD_VALUE_INT, DEVICE_CMD_FLAG_IDEMPOTENT, 1, 180, 1},
    };
    device_cmd_handler_t handlers[] = {cmd_relay,cmd_plug,cmd_light,cmd_dimmer,cmd_fan,cmd_dryer_temp,cmd_drying_time};
    for (size_t i=0; i<sizeof(caps)/sizeof(caps[0]); i++) if (device_command_register_capability(&caps[i], handlers[i]) != 0) return -1;
    return 0;
}

static int product_init(void)
{
    if (demo_gpio_output_init(DEMO_RELAY_GPIO,false) || demo_gpio_output_init(DEMO_PLUG_GPIO,false) || demo_gpio_output_init(DEMO_LIGHT_GPIO,false) || demo_pwm_init(DEMO_DIMMER_PWM_GPIO,0) || demo_pwm_init(DEMO_FAN_PWM_GPIO,1)) return -1;
    if (demo_input_init(DEMO_BUTTON_GPIO, local_button, NULL) != 0 || demo_sensor_start(sensor_sample, NULL) != 0) return -1;
    if (gpio_set_direction(DEMO_CONTACT_GPIO, GPIO_MODE_INPUT) != ESP_OK) return -1;
    return 0;
}
static int product_start(void) { ESP_LOGI(TAG, "started: 7 tools, 10 features"); return 0; }
static int product_stop(void) { return 0; }
static int register_events(void) { return 0; }

static const device_app_profile_t s_profile = {
    .model="esp32s3-demo", .hardware_version="1.0", .firmware_version="0.3.0",
    .ble_name_prefix="GW-DEMO", .protocol_version=GW_PROTOCOL_VERSION, .capability_revision=1,
    .supports_factory_reset=true, .supports_telemetry=true, .supports_local_button=true,
    .product_init=product_init, .product_start=product_start, .product_stop=product_stop,
    .register_commands=register_commands, .register_features=register_features, .register_events=register_events,
};
const device_app_profile_t *demo_product_profile(void) { return &s_profile; }
