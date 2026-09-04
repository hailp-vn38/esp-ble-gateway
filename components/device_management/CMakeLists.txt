idf_component_register(
    SRCS "device_management.c"
    INCLUDE_DIRS "include"
    REQUIRES device_types device_schema
    PRIV_REQUIRES device_store device_state device_command_service
                  ble_central gateway_events
)
