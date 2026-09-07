idf_component_register(
    SRCS "main.c" "reference_product.c" "reference_config.c"
    INCLUDE_DIRS "."
    REQUIRES device_app
             gateway_protocol
             device_command
             device_event
             device_feature
             device_settings
    PRIV_REQUIRES esp_driver_gpio
)
