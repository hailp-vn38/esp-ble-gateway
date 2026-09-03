idf_component_register(
    SRCS "test_device_command_service.c"
    REQUIRES unity device_command_service
    PRIV_REQUIRES esp_timer freertos
)
