idf_component_register(
    SRCS "device_schema_control_adapter.c"
    INCLUDE_DIRS "include"
    REQUIRES device_schema device_control_scheduler device_command_types
)
