idf_component_register(
    SRCS "test_mcp_endpoint.c"
         "test_mcp_conformance.c"
         "test_mcp_stress.c"
         "test_mcp_xiaozhi.c"
         "test_mcp_semantic_control.c"
         "test_mcp_device_control.c"
    REQUIRES unity mcp_endpoint cbor_codec command_dispatcher command_executor
             device_store device_template device_state espressif__cjson
             mcp_tool_exposure
)
