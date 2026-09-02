/*
 * reference_product — Golden Peripheral product logic (docs §53..#57).
 *
 * Hardware: 1 LED (GPIO8) + 1 button (GPIO9, active-low with pull-up).
 * Commands: set_led, get_state.
 * Events: button_pressed, feature_state.
 *
 * Capability registration (spec D6, D7, D8):
 *   - set_led: BOOL, IDEMPOTENT, PUBLIC
 *   - get_state: NONE, IDEMPOTENT, PUBLIC (promoted from internal built-in)
 *
 * Registration order is DETERMINISTIC and becomes PRESENTATION ORDER (spec D8):
 *   1. set_led (sequence 0)
 *   2. get_state (sequence 1)
 *
 * Capability revision: 1 (initial version)
 *   - Increment when public capability schema changes (spec D7)
 *   - Does NOT need to increment when runtime value changes
 */
#include "reference_product.h"

#include <string.h>

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "device_app.h"
#include "device_command.h"
#include "device_event.h"
#include "device_feature.h"

static const char *TAG = "ref_product";

/* ------------------------------------------------------------------ *
 * Hardware pins (docs §54)
 * ------------------------------------------------------------------ */

#define REF_LED_GPIO    GPIO_NUM_8
#define REF_BTN_GPIO    GPIO_NUM_9

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */

static bool s_led_state = false;
static TimerHandle_t s_heartbeat_timer;

static int ref_led_apply(bool new_state, bool publish)
{
    esp_err_t err = gpio_set_level(REF_LED_GPIO, new_state ? 1 : 0);
    if (err != ESP_OK) return -1;

    bool changed = s_led_state != new_state;
    s_led_state = new_state;
    if (publish && changed) {
        int rc = device_feature_publish_bool("led_main", GW_PROP_ON_OFF,
                                             s_led_state);
        if (rc != 0) {
            ESP_LOGW(TAG, "LED state publish failed: %d", rc);
        }
    }
    return 0;
}

static int ref_led_read_state(void *context, bool *out_value)
{
    (void)context;
    if (out_value == NULL) return -1;
    *out_value = s_led_state;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Command handlers
 * ------------------------------------------------------------------ */

static device_cmd_result_t cmd_set_led_handler(
    const gw_message_t *request, device_cmd_response_t *response)
{
    bool new_state = request->bool_value != 0;

    if (ref_led_apply(new_state, true) != 0) {
        response->success = false;
        response->int_value = s_led_state ? 1 : 0;
        return DEVICE_CMD_ERR_HANDLER;
    }

    ESP_LOGI(TAG, "LED -> %s", s_led_state ? "ON" : "OFF");

    response->success = true;
    response->int_value = s_led_state ? 1 : 0;

    device_command_response_set_feature_bool(
        response, "led_main", GW_PROP_ON_OFF, s_led_state);

    return DEVICE_CMD_OK;
}

static device_cmd_result_t cmd_get_state_handler(
    const gw_message_t *request, device_cmd_response_t *response)
{
    (void)request;

    response->success = true;
    response->int_value = s_led_state ? 1 : 0;

    device_command_response_set_feature_bool(
        response, "led_main", GW_PROP_ON_OFF, s_led_state);

    return DEVICE_CMD_OK;
}

/* ------------------------------------------------------------------ *
 * Button ISR + debounce task
 * ------------------------------------------------------------------ */

static volatile bool s_btn_pending = false;

static void IRAM_ATTR btn_isr_handler(void *arg)
{
    s_btn_pending = true;
}

static void btn_task(void *arg)
{
    while (1) {
        if (s_btn_pending) {
            s_btn_pending = false;
            vTaskDelay(pdMS_TO_TICKS(50)); /* debounce */
            if (gpio_get_level(REF_BTN_GPIO) == 0) {
                ESP_LOGI(TAG, "button pressed");
                device_event_publish("button_pressed", DEVICE_EVENT_EDGE, 1, true);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ------------------------------------------------------------------ *
 * Heartbeat timer (optional, doc §56)
 * ------------------------------------------------------------------ */

static void heartbeat_cb(TimerHandle_t timer)
{
    device_event_publish_telemetry("uptime", (int)(xTaskGetTickCount() / 100));
}

/* ------------------------------------------------------------------ *
 * Product lifecycle
 * ------------------------------------------------------------------ */

static int ref_product_init(void)
{
    /* Configure LED. */
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << REF_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_cfg);
    gpio_set_level(REF_LED_GPIO, 0);

    /* Configure button with pull-up + interrupt. */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << REF_BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(REF_BTN_GPIO, btn_isr_handler, NULL);

    /* Start button debounce task. */
    xTaskCreate(btn_task, "btn_task", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "init (LED=GPIO%d, BTN=GPIO%d)", REF_LED_GPIO, REF_BTN_GPIO);
    return 0;
}

static int ref_product_start(void)
{
    /* Start heartbeat timer (every 10s). */
    s_heartbeat_timer = xTimerCreate("heartbeat", pdMS_TO_TICKS(10000),
                                      pdTRUE, NULL, heartbeat_cb);
    if (s_heartbeat_timer) {
        xTimerStart(s_heartbeat_timer, 0);
    }
    ESP_LOGI(TAG, "started");
    return 0;
}

static int ref_product_stop(void)
{
    if (s_heartbeat_timer) {
        xTimerStop(s_heartbeat_timer, 0);
        xTimerDelete(s_heartbeat_timer, 0);
        s_heartbeat_timer = NULL;
    }
    gpio_set_level(REF_LED_GPIO, 0);
    ESP_LOGI(TAG, "stopped");
    return 0;
}

/* ------------------------------------------------------------------ *
 * Command registration (spec D6, D7, D8)
 *
 * Registration order is DETERMINISTIC and becomes PRESENTATION ORDER.
 * Built-in commands (ping, get_info) are INTERNAL by default.
 * get_state is PROMOTED from internal built-in to PUBLIC here.
 * ------------------------------------------------------------------ */

static int ref_register_commands(void)
{
    const device_cmd_capability_t set_led = {
        .command = "set_led",
        .label = "LED power",
        .unit = "",
        .value_type = DEVICE_CMD_VALUE_BOOL,
        .flags = DEVICE_CMD_FLAG_IDEMPOTENT,
    };
    const device_cmd_capability_t get_state = {
        .command = "get_state",
        .label = "LED state",
        .unit = "",
        .value_type = DEVICE_CMD_VALUE_NONE,
        .flags = DEVICE_CMD_FLAG_IDEMPOTENT,
    };
    /* Register in deterministic order: set_led first, get_state second.
     * This order is frozen and becomes PRESENTATION ORDER (spec D8). */
    if (device_command_register_capability(&set_led,
                                           cmd_set_led_handler) != 0 ||
        device_command_register_capability(&get_state,
                                           cmd_get_state_handler) != 0) {
        return -1;
    }
    ESP_LOGI(TAG, "commands registered: set_led, get_state");
    return 0;
}

static int ref_register_events(void)
{
    ESP_LOGI(TAG, "events registered: button_pressed, feature_state, heartbeat");
    return 0;
}

static int ref_register_features(void)
{
    const device_feature_on_off_light_config_t config = {
        .feature_id = "led_main",
        .set_command = "set_led",
        .read_on_off = ref_led_read_state,
        .context = NULL,
    };
    return device_feature_register_on_off_light(&config);
}

/* ------------------------------------------------------------------ *
 * Profile
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Profile (spec D7, §17)
 *
 * Capability revision: 1 (initial version)
 *   - Increment when public capability schema changes (spec D7)
 *   - Does NOT need to increment when runtime value changes
 * ------------------------------------------------------------------ */

static const device_app_profile_t s_profile = {
    .model = "esp32s3-ref",
    .hardware_version = "1.0",
    .firmware_version = "0.2.0",
    .ble_name_prefix = "GW-REF",
    .protocol_version = GW_PROTOCOL_VERSION,
    .capability_revision = 2,  /* v4 semantic LED feature added */
    .supports_factory_reset = true,
    .supports_telemetry = true,
    .supports_local_button = true,
    .product_init = ref_product_init,
    .product_start = ref_product_start,
    .product_stop = ref_product_stop,
    .register_commands = ref_register_commands,
    .register_features = ref_register_features,
    .register_events = ref_register_events,
};

const device_app_profile_t *reference_product_profile(void)
{
    return &s_profile;
}
