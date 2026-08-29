idf_component_register(
    SRCS "mcp_endpoint.c"
         "mcp_core.c"
         "mcp_rpc.c"
         "mcp_tools.c"
         "mcp_registry.c"
         "mcp_auth.c"
         "mcp_codec.c"
         "mcp_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server cbor_codec command_dispatcher command_executor
             device_capabilities device_store mcp_tool_exposure
             espressif__cjson nvs_flash esp_timer freertos
    PRIV_REQUIRES memory_policy
)
