idf_component_register(
    SRCS "device_schema.c" "device_schema_store.c" "device_schema_validate.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec device_store nvs_flash esp_timer freertos
             memory_policy
)
