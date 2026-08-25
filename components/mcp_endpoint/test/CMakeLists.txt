idf_component_register(
    SRCS "test_mcp_endpoint.c"
    REQUIRES unity mcp_endpoint cbor_codec command_dispatcher espressif__cjson
)
