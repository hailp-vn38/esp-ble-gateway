idf_component_register(
    SRCS "device_command_service.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec memory_policy
    PRIV_REQUIRES esp_timer freertos
)
