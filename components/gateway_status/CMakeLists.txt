idf_component_register(
    SRCS "gateway_status.c"
    INCLUDE_DIRS "include"
    REQUIRES device_store wifi_provisioning ble_central esp_wifi
             esp_app_format esp_timer esp_common memory_policy
    PRIV_REQUIRES esp_system esp_psram
)
