idf_component_register(
    SRCS "wifi_prov.c" "dns_hijack.c" "dns_packet.c" "captive_portal.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event nvs_flash esp_netif lwip
)
