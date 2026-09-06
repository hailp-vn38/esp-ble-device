#ifndef DEMO_PWM_H
#define DEMO_PWM_H
#include "driver/gpio.h"
int demo_pwm_init(gpio_num_t pin, int channel);
int demo_pwm_set(gpio_num_t pin, int channel, int value);
#endif
