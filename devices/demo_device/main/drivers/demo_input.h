#ifndef DEMO_INPUT_H
#define DEMO_INPUT_H
#include <stdbool.h>
#include "driver/gpio.h"
typedef void (*demo_input_cb_t)(void *context);
int demo_input_init(gpio_num_t pin, demo_input_cb_t callback, void *context);
bool demo_input_pressed(gpio_num_t pin);
#endif
