#include "demo_pwm.h"
#include "driver/ledc.h"
int demo_pwm_init(gpio_num_t pin, int channel) {
    ledc_timer_config_t timer = {.speed_mode=LEDC_LOW_SPEED_MODE,.timer_num=LEDC_TIMER_0,.duty_resolution=LEDC_TIMER_10_BIT,.freq_hz=1000,.clk_cfg=LEDC_AUTO_CLK};
    if (ledc_timer_config(&timer) != ESP_OK) return -1;
    ledc_channel_config_t cfg = {.gpio_num=pin,.speed_mode=LEDC_LOW_SPEED_MODE,.channel=channel,.intr_type=LEDC_INTR_DISABLE,.timer_sel=LEDC_TIMER_0,.duty=0,.hpoint=0};
    return ledc_channel_config(&cfg) == ESP_OK ? 0 : -1;
}
int demo_pwm_set(gpio_num_t pin, int channel, int value) {
    (void)pin;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    uint32_t duty = (uint32_t)value * 1023 / 100;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty) != ESP_OK) return -1;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, channel) == ESP_OK ? 0 : -1;
}
