idf_component_register(
    SRCS "command_dispatcher.c"
         "command_registry.c"
         "gateway_commands.c"
         "device_command.c"
         "device_request_manager.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES cbor_codec ble_central device_store device_schema device_state device_template
             gateway_status freertos mcp_tool_exposure gateway_events
             espressif__cjson
)
