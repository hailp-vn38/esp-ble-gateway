idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES device_store wifi_provisioning ble_central cbor_codec
             command_dispatcher web_server mcp_endpoint log_buffer
             nvs_flash esp_wifi esp_event bt esp_http_server
)
