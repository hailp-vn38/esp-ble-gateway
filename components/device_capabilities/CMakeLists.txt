idf_component_register(
    SRCS "device_capabilities.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec device_store nvs_flash esp_timer freertos
             memory_policy
)
