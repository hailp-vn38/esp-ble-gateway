idf_component_register(
    SRCS "test_captive_http.c"
    INCLUDE_DIRS "." "../"
    PRIV_REQUIRES unity web_server esp_netif lwip
)
