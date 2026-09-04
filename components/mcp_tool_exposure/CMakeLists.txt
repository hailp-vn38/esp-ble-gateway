idf_component_register(
    SRCS "mcp_tool_exposure.c"
         "mcp_tool_exposure_store.c"
         "mcp_tool_digest.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES device_store device_schema device_template nvs_flash mbedtls
             espressif__cjson freertos esp_timer memory_policy
)
