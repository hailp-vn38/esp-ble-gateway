idf_component_register(
    SRCS "device_schema.c" "device_schema_runtime.c" "device_schema_worker.c" "device_schema_protocol.c" "device_schema_discovery.c" "device_schema_store.c" "device_schema_validate.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec device_types device_store nvs_flash esp_timer freertos
             memory_policy gateway_events
)
