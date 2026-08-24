idf_component_register(
    SRCS "command_dispatcher.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec ble_central device_store log_buffer freertos espressif__cjson
)
