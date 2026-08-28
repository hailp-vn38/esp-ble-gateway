idf_component_register(
    SRCS "command_dispatcher.c"
         "command_registry.c"
         "gateway_commands.c"
         "device_command.c"
         "device_request_manager.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES cbor_codec ble_central device_store device_capabilities
             gateway_status freertos mcp_tool_exposure
             espressif__cjson
)
