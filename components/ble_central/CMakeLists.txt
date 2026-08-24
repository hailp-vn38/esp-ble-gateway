idf_component_register(
    SRCS "ble_central.c"
    INCLUDE_DIRS "include"
    REQUIRES bt cbor_codec device_store
)
