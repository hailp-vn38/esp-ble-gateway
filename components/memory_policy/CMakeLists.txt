idf_component_register(
    SRCS "memory_policy.c" "cjson_allocator.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_psram
    REQUIRES espressif__cjson
)
