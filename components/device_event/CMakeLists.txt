idf_component_register(
    SRCS "device_event.c"
    INCLUDE_DIRS "include"
    REQUIRES gateway_protocol
               ble_peripheral
)
