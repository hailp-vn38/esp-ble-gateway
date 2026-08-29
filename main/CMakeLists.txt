idf_component_register(
    SRCS "main.c"
         "board_status_sync.c"
         "gateway_ota_validate.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_psram
    REQUIRES device_store device_capabilities wifi_provisioning ble_central cbor_codec
             command_dispatcher command_executor web_server mcp_endpoint
             mcp_tool_exposure mcp_ws_bridge
             nvs_flash esp_wifi esp_event bt esp_http_server
             board_io app_update
)
