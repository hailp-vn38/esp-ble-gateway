idf_component_register(
    SRCS "web_auth.c"
         "web_auth_store.c"
         "web_auth_password.c"
         "web_auth_session.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash mbedtls esp_timer esp_system freertos
)
