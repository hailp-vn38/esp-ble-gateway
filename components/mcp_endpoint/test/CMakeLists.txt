idf_component_register(
    SRCS "test_mcp_endpoint.c"
         "test_mcp_conformance.c"
         "test_mcp_stress.c"
    REQUIRES unity mcp_endpoint cbor_codec command_dispatcher espressif__cjson
)
