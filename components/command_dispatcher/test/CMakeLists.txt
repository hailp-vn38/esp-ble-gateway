idf_component_register(
    SRCS "test_command_dispatcher.c"
         "test_device_request_manager.c"
    REQUIRES unity command_dispatcher espressif__cjson
)
