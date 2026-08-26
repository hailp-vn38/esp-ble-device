idf_component_register(
    SRCS "main.c" "reference_product.c"
    INCLUDE_DIRS "."
    REQUIRES device_app
             gateway_protocol
             device_command
             device_event
    PRIV_REQUIRES esp_driver_gpio
)
