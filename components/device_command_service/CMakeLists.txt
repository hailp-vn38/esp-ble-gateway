idf_component_register(
    SRCS "device_command_service.c"
         "device_command_request.c"
         "device_command_pending.c"
         "device_command_worker.c"
    INCLUDE_DIRS "include"
    REQUIRES cbor_codec memory_policy device_schema
    PRIV_REQUIRES esp_timer freertos
)
