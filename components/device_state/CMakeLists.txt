idf_component_register(
    SRCS "device_state.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec device_schema command_executor esp_timer freertos
)
