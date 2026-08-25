idf_component_register(
    SRCS "mcp_endpoint.c"
         "mcp_rpc.c"
         "mcp_tools.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server cbor_codec command_dispatcher espressif__cjson
)
