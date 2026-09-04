idf_component_register(
    SRCS "test_device_management.c"
    PRIV_INCLUDE_DIRS ".."
    REQUIRES unity device_management device_store ble_central gateway_events
             esp_timer
)
