idf_component_register(
    SRCS "command_executor.c"
    INCLUDE_DIRS "include"
    REQUIRES command_dispatcher freertos esp_timer
    PRIV_REQUIRES log
)
