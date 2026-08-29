idf_component_register(
    SRCS "test_mcp_endpoint.c"
         "test_mcp_conformance.c"
         "test_mcp_stress.c"
         "test_mcp_xiaozhi.c"
    REQUIRES unity mcp_endpoint cbor_codec command_dispatcher command_executor
             espressif__cjson
)
