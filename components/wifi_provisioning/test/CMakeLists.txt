idf_component_register(
    SRCS "test_dns_parser.c" "test_dns_lifecycle.c"
    INCLUDE_DIRS "."
    REQUIRES unity wifi_provisioning esp_netif lwip
)
