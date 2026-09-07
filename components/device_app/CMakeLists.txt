idf_component_register(
    SRCS "device_app.c"
    INCLUDE_DIRS "include"
    REQUIRES gateway_protocol
               ble_peripheral
               device_command
               device_event
               device_feature
               device_settings

)
