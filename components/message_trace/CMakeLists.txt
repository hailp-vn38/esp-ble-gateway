idf_component_register(
    SRCS "message_trace.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec freertos log esp_timer
)
