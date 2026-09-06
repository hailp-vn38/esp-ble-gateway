idf_component_register(
    SRCS "device_settings.c" "device_settings_memory.c"
         "device_settings_protocol.c" "device_settings_operation.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec device_types device_store nvs_flash esp_timer
             freertos memory_policy gateway_events
)
