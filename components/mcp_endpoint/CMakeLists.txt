idf_component_register(
    SRCS "mcp_endpoint.c"
         "mcp_core.c"
         "mcp_rpc.c"
         "mcp_tools.c"
         "mcp_registry.c"
         "mcp_auth.c"
         "mcp_codec.c"
         "mcp_policy.c"
         "mcp_device_control.c"
         "mcp_semantic_control.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server cbor_codec
             device_command_service device_schema device_state device_template
             device_store gateway_status mcp_tool_exposure espressif__cjson
             nvs_flash esp_timer freertos
    PRIV_REQUIRES memory_policy
)
