#include "demo_input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static volatile bool s_pending;
static demo_input_cb_t s_callback;
static void *s_context;
static void IRAM_ATTR demo_input_isr(void *arg) { (void)arg; s_pending = true; }
static void demo_input_task(void *arg) {
    gpio_num_t pin = (gpio_num_t)(intptr_t)arg;
    for (;;) {
        if (s_pending) {
            s_pending = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            if (demo_input_pressed(pin) && s_callback) s_callback(s_context);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
int demo_input_init(gpio_num_t pin, demo_input_cb_t callback, void *context) {
    gpio_config_t cfg = {.pin_bit_mask=1ULL << pin,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE,.intr_type=GPIO_INTR_NEGEDGE};
    if (gpio_config(&cfg) != ESP_OK) return -1;
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) return -1;
    if (gpio_isr_handler_add(pin, demo_input_isr, NULL) != ESP_OK) return -1;
    s_callback = callback; s_context = context;
    return xTaskCreate(demo_input_task, "demo_input", 2048, (void *)(intptr_t)pin, 3, NULL) == pdPASS ? 0 : -1;
}
bool demo_input_pressed(gpio_num_t pin) { return gpio_get_level(pin) == 0; }
