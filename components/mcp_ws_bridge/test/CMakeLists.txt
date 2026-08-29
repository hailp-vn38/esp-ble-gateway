idf_component_register(
    SRCS "test_mcp_ws_bridge_config.c"
         "test_mcp_ws_bridge_lifecycle.c"
    INCLUDE_DIRS "."
    REQUIRES unity mcp_ws_bridge nvs_flash
)
