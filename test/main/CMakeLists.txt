idf_component_register(
    SRCS "test_main.c"
    INCLUDE_DIRS "."
    REQUIRES unity nvs_flash device_store log_buffer
)
