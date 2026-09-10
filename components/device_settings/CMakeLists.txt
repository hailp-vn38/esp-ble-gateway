idf_component_register(
    SRCS "device_settings.c" "device_settings_memory.c" "device_settings_events.c"
         "device_settings_protocol.c" "device_settings_operation.c"
         "device_settings_transaction.c" "device_settings_worker.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec device_types device_store nvs_flash esp_timer
             freertos memory_policy gateway_events device_control_scheduler
)
