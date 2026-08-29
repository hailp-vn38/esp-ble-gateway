idf_component_register(
    SRCS "mcp_ws_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES mcp_endpoint wifi_provisioning esp_websocket_client esp_event
             esp-tls mbedtls nvs_flash freertos esp_timer espressif__cjson
             esp_wifi esp_netif esp_hw_support
)
