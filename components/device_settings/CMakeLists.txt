idf_component_register(
    SRCS "device_settings_registry.c" "device_settings_transaction.c"
         "device_settings_persistence.c" "device_settings_restart.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)