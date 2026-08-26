idf_component_register(
    SRCS "mcp_endpoint.c"
         "mcp_rpc.c"
         "mcp_tools.c"
         "mcp_registry.c"
         "mcp_auth.c"
         "mcp_codec.c"
         "mcp_async.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server cbor_codec command_dispatcher espressif__cjson nvs_flash esp_timer freertos
)
