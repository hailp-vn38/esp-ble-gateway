idf_component_register(
    SRCS
        "test_ble_state.c"
        "test_ble_runtime.c"
        "test_ble_scheduler.c"
        "test_ble_removal.c"
        "test_ble_host_reset.c"
        "test_ble_notify.c"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/.."
    REQUIRES unity ble_central cbor_codec
)
