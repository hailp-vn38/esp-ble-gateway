idf_component_register(
    SRCS "gateway_events.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec freertos
)
