idf_component_register(
    SRCS "test_main.c"
    INCLUDE_DIRS "."
    REQUIRES unity nvs_flash esp_netif device_store device_schema device_state
             mcp_tool_exposure
)
