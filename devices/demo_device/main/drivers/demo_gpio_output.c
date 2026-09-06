#include "demo_gpio_output.h"
int demo_gpio_output_init(gpio_num_t pin, bool initial) {
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&cfg) != ESP_OK) return -1;
    return demo_gpio_output_set(pin, initial);
}
int demo_gpio_output_set(gpio_num_t pin, bool value) {
    return gpio_set_level(pin, value ? 1 : 0) == ESP_OK ? 0 : -1;
}
