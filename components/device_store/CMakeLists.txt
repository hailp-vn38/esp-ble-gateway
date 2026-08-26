idf_component_register(
    SRCS "device_store.c"
         "device_store_entry.c"
         "device_store_migration.c"
         "device_store_nvs.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)
