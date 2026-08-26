idf_component_register(
    SRCS "main.c"
         "board_status_sync.c"
    INCLUDE_DIRS "."
    REQUIRES device_store device_capabilities wifi_provisioning ble_central cbor_codec
             command_dispatcher command_executor web_server mcp_endpoint
             log_buffer nvs_flash esp_wifi esp_event bt esp_http_server
             board_io
)
