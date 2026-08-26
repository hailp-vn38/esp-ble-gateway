#include <string.h>

#include "unity.h"

#include "ble_central_internal.h"

#include "test_ble_common.h"

TEST_CASE("host reset frees all slots and clears mappings", "[ble_host_reset]")
{
    ble_test_bootstrap();

    int connected = ble_test_register("hr-connected");
    int connecting = ble_test_register("hr-connecting");
    int backoff = ble_test_register("hr-backoff");
    int offline = ble_test_register("hr-offline");
    (void)offline;

    ble_conn_ref_t ref_ready;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(connected, &ref_ready, 1000));
    TEST_ASSERT_TRUE(ble_state_on_connect_success(ref_ready, 21, 1100));

    ble_conn_ref_t ref_backoff;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(backoff, &ref_backoff, 1001));
    ble_scheduler_note_failure(backoff, 2000);
    TEST_ASSERT_EQUAL_INT(BLE_DEVICE_BACKOFF, ble_test_dev(backoff).state);

    ble_conn_ref_t ref_connecting;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(connecting, &ref_connecting, 1002));

    uint32_t gen_before = ble_test_generation_of(ref_ready.slot_index);

    char ids[DEVICE_STORE_MAX_DEVICES][GW_MSG_DEVICE_ID_LEN];
    size_t mirrored =
        ble_state_handle_host_reset(ids, DEVICE_STORE_MAX_DEVICES);

    TEST_ASSERT_EQUAL_size_t(2, mirrored);
    bool saw_connected = false;
    bool saw_connecting = false;
    for (size_t i = 0; i < mirrored; i++) {
        if (strcmp(ids[i], "hr-connected") == 0) saw_connected = true;
        if (strcmp(ids[i], "hr-connecting") == 0) saw_connecting = true;
    }
    TEST_ASSERT_TRUE(saw_connected);
    TEST_ASSERT_TRUE(saw_connecting);

    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        TEST_ASSERT_TRUE(ble_state_lock());
        TEST_ASSERT_EQUAL_INT(BLE_CONN_FREE, g_ble_connections[i].state);
        TEST_ASSERT_EQUAL_INT(-1, g_ble_connections[i].device_index);
        TEST_ASSERT_EQUAL_INT(BLE_HS_CONN_HANDLE_NONE,
                              g_ble_connections[i].conn_handle);
        ble_state_unlock();
    }

    TEST_ASSERT_EQUAL_INT(-1, ble_test_dev(connected).connection_slot);
    TEST_ASSERT_EQUAL_INT(-1, ble_test_dev(connecting).connection_slot);
    TEST_ASSERT_EQUAL_INT(BLE_DEVICE_OFFLINE, ble_test_dev(connected).state);
    TEST_ASSERT_EQUAL_INT(BLE_DEVICE_OFFLINE, ble_test_dev(connecting).state);
    TEST_ASSERT_EQUAL_INT(BLE_DEVICE_OFFLINE, ble_test_dev(backoff).state);
    TEST_ASSERT_EQUAL_INT64(0, ble_test_dev(backoff).next_retry_ms);
    TEST_ASSERT_EQUAL_UINT32(gen_before + 1,
                             ble_test_generation_of(ref_ready.slot_index));

    ble_conn_slot_t stale_snap;
    TEST_ASSERT_FALSE(ble_conn_snapshot(ref_connecting, &stale_snap));
}

TEST_CASE("devices can reconnect after host reset", "[ble_host_reset]")
{
    ble_test_bootstrap();

    int idx = ble_test_register("hr-reconnect");
    ble_addr_t addr = {.type = 0, .val = {1, 2, 3, 4, 5, 6}};
    TEST_ASSERT_TRUE(ble_runtime_set_peer_addr(idx, &addr));

    ble_conn_ref_t first;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(idx, &first, 1000));
    uint32_t gen_first = ble_test_generation_of(first.slot_index);
    TEST_ASSERT_EQUAL_UINT32(gen_first, first.generation);

    char ids[DEVICE_STORE_MAX_DEVICES][GW_MSG_DEVICE_ID_LEN];
    TEST_ASSERT_EQUAL_size_t(1, ble_state_handle_host_reset(ids, DEVICE_STORE_MAX_DEVICES));

    ble_conn_ref_t again;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(idx, &again, 2000));
    TEST_ASSERT_EQUAL_INT(first.slot_index, again.slot_index);
    TEST_ASSERT_EQUAL_UINT32(gen_first + 2, again.generation);
}
