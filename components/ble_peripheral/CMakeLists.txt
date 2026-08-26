idf_component_register(
    SRCS "ble_peripheral.c"
    INCLUDE_DIRS "include"
    REQUIRES gateway_protocol
               bt
               nvs_flash
)
