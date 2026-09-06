idf_component_register(
    SRCS "main.c" "demo_product.c"
         "drivers/demo_gpio_output.c" "drivers/demo_pwm.c"
         "drivers/demo_input.c" "drivers/demo_sensor.c"
    INCLUDE_DIRS "." "drivers"
    REQUIRES device_app gateway_protocol device_command device_event device_feature
    PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_timer freertos
)
