idf_component_register(
    SRCS "test_captive_http.c" "test_event_ws.c"
         "test_web_api_baseline.c"
    INCLUDE_DIRS "." "../"
    PRIV_REQUIRES unity web_server esp_netif lwip gateway_events
                  command_dispatcher device_store
)
