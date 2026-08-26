#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ble_central_internal.h"

#include "test_ble_common.h"

TEST_CASE("connection pool reserves all slots and rejects overflow", "[ble_state]")
{
    ble_test_bootstrap();

    char id[16];
    ble_conn_ref_t refs[BLE_CENTRAL_MAX_CONN];
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        snprintf(id, sizeof(id), "dev-%d", i);
        int idx = ble_test_register(id);
        TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                              ble_state_reserve_connection(idx, &refs[i], 1000));
        TEST_ASSERT_TRUE(ble_state_on_connect_success(refs[i],
                                                      (uint16_t)(100 + i),
                                                      1001));
    }

    int extra = ble_test_register("dev-overflow");
    ble_conn_ref_t unused_ref;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_ERR_NO_SLOT,
                          ble_state_reserve_connection(extra, &unused_ref, 1000));
}

TEST_CASE("rollback releases slot, schedules backoff and keeps generation", "[ble_state]")
{
    ble_test_bootstrap();

    int idx = ble_test_register("dev-rb");
    ble_conn_ref_t ref;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(idx, &ref, 1000));

    uint32_t allocated_gen = ble_test_generation_of(ref.slot_index);

    ble_state_rollback_connection_start(ref, 5000);

    TEST_ASSERT_TRUE(ble_state_lock());
    TEST_ASSERT_EQUAL_INT(BLE_CONN_FREE, g_ble_connections[ref.slot_index].state);
    TEST_ASSERT_EQUAL_UINT32(allocated_gen,
                             g_ble_connections[ref.slot_index].generation);
    ble_state_unlock();

    ble_device_runtime_t snapshot;
    TEST_ASSERT_TRUE(ble_runtime_snapshot(idx, &snapshot));
    TEST_ASSERT_EQUAL_INT(-1, snapshot.connection_slot);
    TEST_ASSERT_EQUAL_INT(BLE_DEVICE_BACKOFF, snapshot.state);
    TEST_ASSERT_EQUAL_UINT8(1, snapshot.retry_count);
    TEST_ASSERT_EQUAL_INT64(5000 + BLE_RETRY_INITIAL_MS, snapshot.next_retry_ms);

    ble_conn_ref_t reused;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(idx, &reused, 6000));
    TEST_ASSERT_EQUAL_INT(ref.slot_index, reused.slot_index);
    TEST_ASSERT_EQUAL_UINT32(allocated_gen + 1, reused.generation);
}

TEST_CASE("reserve rejects second concurrent connect procedure", "[ble_state]")
{
    ble_test_bootstrap();

    int a = ble_test_register("dev-a");
    int b = ble_test_register("dev-b");

    ble_conn_ref_t ref_a;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(a, &ref_a, 1000));

    ble_conn_ref_t ref_b;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_ERR_CONNECT_IN_PROGRESS,
                          ble_state_reserve_connection(b, &ref_b, 1000));

    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_ERR_BUSY,
                          ble_state_reserve_connection(a, &ref_b, 1000));
}

TEST_CASE("connect success and disconnect complete one lifecycle", "[ble_state]")
{
    ble_test_bootstrap();

    uint32_t disconnects_before = ble_central_metrics()->disconnects;

    int idx = ble_test_register("dev-life");
    ble_conn_ref_t ref;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(idx, &ref, 1000));

    TEST_ASSERT_TRUE(ble_state_on_connect_success(ref, 7, 1100));

    ble_conn_slot_t snap;
    TEST_ASSERT_TRUE(ble_conn_snapshot(ref, &snap));
    TEST_ASSERT_EQUAL_INT(BLE_CONN_SECURING, snap.state);
    TEST_ASSERT_EQUAL_UINT16(7, snap.conn_handle);
    TEST_ASSERT_EQUAL_INT64(1100, snap.started_ms);

    uint16_t discovered = ble_state_begin_discovery(ref, 1200);
    TEST_ASSERT_EQUAL_UINT16(7, discovered);
    TEST_ASSERT_TRUE(ble_conn_snapshot(ref, &snap));
    TEST_ASSERT_EQUAL_INT(BLE_CONN_DISCOVERING, snap.state);
    TEST_ASSERT_EQUAL_INT64(1200, snap.discovery_started_ms);

    ble_state_set_ready(ref);
    TEST_ASSERT_TRUE(ble_conn_snapshot(ref, &snap));
    TEST_ASSERT_EQUAL_INT(BLE_CONN_READY, snap.state);

    ble_state_update_mtu((ble_conn_event_ref_t){.ref = ref, .conn_handle = 7},
                         251);
    TEST_ASSERT_TRUE(ble_conn_snapshot(ref, &snap));
    TEST_ASSERT_EQUAL_UINT16(251, snap.mtu);

    TEST_ASSERT_TRUE(ble_state_on_disconnect(
        (ble_conn_event_ref_t){.ref = ref, .conn_handle = 7}, 2000, NULL, 0,
        NULL, NULL, NULL));

    TEST_ASSERT_FALSE(ble_conn_snapshot(ref, &snap));
    TEST_ASSERT_TRUE(ble_state_lock());
    TEST_ASSERT_EQUAL_INT(BLE_HS_CONN_HANDLE_NONE,
                          g_ble_connections[ref.slot_index].conn_handle);
    ble_state_unlock();
    ble_device_runtime_t dev = ble_test_dev(idx);
    TEST_ASSERT_EQUAL_INT(-1, dev.connection_slot);
    TEST_ASSERT_EQUAL_INT(BLE_DEVICE_BACKOFF, dev.state);
    TEST_ASSERT_EQUAL_UINT32(disconnects_before + 1,
                             ble_central_metrics()->disconnects);
}

TEST_CASE("stale generation callbacks are rejected and counted", "[ble_state]")
{
    ble_test_bootstrap();

    int idx = ble_test_register("dev-stale");
    ble_conn_ref_t ref;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(idx, &ref, 1000));
    TEST_ASSERT_TRUE(ble_state_on_connect_success(ref, 9, 1100));

    char ids[DEVICE_STORE_MAX_DEVICES][GW_MSG_DEVICE_ID_LEN];
    TEST_ASSERT_EQUAL_size_t(1,
                             ble_state_handle_host_reset(ids, DEVICE_STORE_MAX_DEVICES));
    TEST_ASSERT_EQUAL_STRING("dev-stale", ids[0]);

    uint32_t stale_before = ble_central_metrics()->stale_callbacks;
    ble_conn_event_ref_t stale_event = {.ref = ref, .conn_handle = 9};
    TEST_ASSERT_FALSE(ble_state_on_disconnect(stale_event, 3000, NULL, 0,
                                              NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(stale_before + 1,
                             ble_central_metrics()->stale_callbacks);

    ble_state_update_mtu(stale_event, 251);
    TEST_ASSERT_EQUAL_UINT32(stale_before + 2,
                             ble_central_metrics()->stale_callbacks);
}

TEST_CASE("mtu updates with wrong handle are rejected", "[ble_state]")
{
    ble_test_bootstrap();

    int idx = ble_test_register("dev-mtu");
    ble_conn_ref_t ref;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(idx, &ref, 1000));
    TEST_ASSERT_TRUE(ble_state_on_connect_success(ref, 5, 1100));

    ble_state_update_mtu((ble_conn_event_ref_t){.ref = ref, .conn_handle = 6},
                         251);

    ble_conn_slot_t snap;
    TEST_ASSERT_TRUE(ble_conn_snapshot(ref, &snap));
    TEST_ASSERT_EQUAL_UINT16(23, snap.mtu);
}

TEST_CASE("backoff delay doubles up to the maximum", "[ble_state]")
{
    ble_test_bootstrap();

    TEST_ASSERT_EQUAL_INT64(2000, ble_backoff_delay_ms(0));
    TEST_ASSERT_EQUAL_INT64(4000, ble_backoff_delay_ms(1));
    TEST_ASSERT_EQUAL_INT64(8000, ble_backoff_delay_ms(2));
    TEST_ASSERT_EQUAL_INT64(16000, ble_backoff_delay_ms(3));
    TEST_ASSERT_EQUAL_INT64(30000, ble_backoff_delay_ms(4));
    TEST_ASSERT_EQUAL_INT64(30000, ble_backoff_delay_ms(10));
}
