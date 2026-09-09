idf_component_register(
    SRCS "test_device_control_scheduler.c"
    REQUIRES unity device_control_scheduler device_command_service
    PRIV_REQUIRES freertos
)
