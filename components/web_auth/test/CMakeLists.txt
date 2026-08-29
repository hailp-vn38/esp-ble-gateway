idf_component_register(
    SRCS "test_web_auth.c"
    INCLUDE_DIRS "." ".."
    REQUIRES web_auth unity esp_timer
)
