idf_component_register(
    SRCS "device_state.c" "device_state_seed.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec device_types device_schema device_control_scheduler device_command_types esp_timer freertos gateway_events
)
