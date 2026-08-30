idf_component_register(
    SRCS "command_executor.c"
    INCLUDE_DIRS "include"
    REQUIRES command_dispatcher freertos esp_timer memory_policy
    PRIV_REQUIRES log
)
