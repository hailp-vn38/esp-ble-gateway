idf_component_register(
    SRCS "test_device_schema.c"
    REQUIRES unity device_schema device_store cbor_codec freertos nvs_flash
)
