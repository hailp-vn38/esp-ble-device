#ifndef DEMO_GPIO_OUTPUT_H
#define DEMO_GPIO_OUTPUT_H
#include <stdbool.h>
#include "driver/gpio.h"
int demo_gpio_output_init(gpio_num_t pin, bool initial);
int demo_gpio_output_set(gpio_num_t pin, bool value);
#endif
