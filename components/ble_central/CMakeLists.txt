idf_component_register(
    SRCS
        "ble_central.c"
        "ble_central_gap.c"
        "ble_central_gatt.c"
        "ble_central_identity.c"
        "ble_central_notify.c"
        "ble_central_runtime.c"
        "ble_central_scan.c"
        "ble_central_state.c"
        "ble_central_supervisor.c"
    INCLUDE_DIRS "include"
    REQUIRES bt cbor_codec device_store message_trace
)
