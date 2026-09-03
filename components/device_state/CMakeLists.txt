idf_component_register(
    SRCS "device_state.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec device_schema device_command_service esp_timer freertos gateway_events
)
