idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server cbor_codec command_dispatcher device_store log_buffer
             wifi_provisioning ble_central espressif__cjson esp_system esp_timer freertos
)
