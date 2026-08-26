idf_component_register(
    SRCS "device_command.c"
    INCLUDE_DIRS "include"
    REQUIRES gateway_protocol
               ble_peripheral
)
