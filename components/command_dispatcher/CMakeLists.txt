idf_component_register(
    SRCS "command_dispatcher.c"
         "command_registry.c"
         "gateway_commands.c"
         "device_command.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec ble_central device_store freertos espressif__cjson
)
