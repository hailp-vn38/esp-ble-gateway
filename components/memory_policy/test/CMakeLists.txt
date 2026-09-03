idf_component_register(
    SRCS "test_memory_policy.c"
    INCLUDE_DIRS "."
    REQUIRES memory_policy unity esp_psram espressif__cjson
)
