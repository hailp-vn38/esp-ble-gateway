#include <string.h>

#include "unity.h"

#include "ble_central_internal.h"

#include "test_ble_common.h"

static void make_eligible(int device_index)
{
    ble_addr_t addr = {.type = 0, .val = {1, 2, 3, 4, 5, 6}};
    TEST_ASSERT_TRUE(ble_runtime_set_peer_addr(device_index, &addr));
}

TEST_CASE("scheduler round-robin cycles through eligible devices", "[ble_scheduler]")
{
    ble_test_bootstrap();

    int a = ble_test_register("sch-a");
    int b = ble_test_register("sch-b");
    int c = ble_test_register("sch-c");
    make_eligible(a);
    make_eligible(b);
    make_eligible(c);

    int first[3];
    int sum = 0;
    for (int i = 0; i < 3; i++) {
        first[i] = ble_scheduler_next_device(1000);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, first[i]);
        sum += first[i];
    }
    TEST_ASSERT_EQUAL_INT(a + b + c, sum);

    int wrapped = ble_scheduler_next_device(1000);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, wrapped);
    TEST_ASSERT_EQUAL_INT(first[0], wrapped);
}

TEST_CASE("scheduler skips ineligible devices", "[ble_scheduler]")
{
    ble_test_bootstrap();

    int only = ble_test_register("sch-only");
    make_eligible(only);

    TEST_ASSERT_EQUAL_INT(only, ble_scheduler_next_device(1000));

    TEST_ASSERT_TRUE(ble_state_lock());
    g_ble_devices[only].state = BLE_DEVICE_REMOVING;
    ble_state_unlock();
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_ERR_NOT_FOUND,
                          ble_scheduler_next_device(1000));

    TEST_ASSERT_TRUE(ble_state_lock());
    g_ble_devices[only].state = BLE_DEVICE_OFFLINE;
    g_ble_devices[only].reconnect_enabled = false;
    ble_state_unlock();
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_ERR_NOT_FOUND,
                          ble_scheduler_next_device(1000));

    TEST_ASSERT_TRUE(ble_state_lock());
    g_ble_devices[only].reconnect_enabled = true;
    g_ble_devices[only].has_peer_addr = false;
    ble_state_unlock();
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_ERR_NOT_FOUND,
                          ble_scheduler_next_device(1000));
}

TEST_CASE("scheduler honors backoff deadline before selecting", "[ble_scheduler]")
{
    ble_test_bootstrap();

    int idx = ble_test_register("sch-backoff");
    make_eligible(idx);

    ble_conn_ref_t ref;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(idx, &ref, 9000));
    ble_scheduler_note_failure(idx, 10000);
    TEST_ASSERT_EQUAL_INT(BLE_DEVICE_BACKOFF, ble_test_dev(idx).state);
    TEST_ASSERT_EQUAL_INT(-1, ble_test_dev(idx).connection_slot);

    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_ERR_NOT_FOUND,
                          ble_scheduler_next_device(11999));
    TEST_ASSERT_EQUAL_INT(idx, ble_scheduler_next_device(12000));
}

TEST_CASE("note failure progresses backoff and success resets it", "[ble_scheduler]")
{
    ble_test_bootstrap();

    int idx = ble_test_register("sch-progress");
    make_eligible(idx);

    static const int64_t expected_delays[] = {2000, 4000, 8000, 16000, 30000,
                                              30000};
    int64_t now = 100000;
    for (size_t i = 0; i < sizeof(expected_delays) / sizeof(expected_delays[0]);
         i++) {
        ble_conn_ref_t ref;
        TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                              ble_state_reserve_connection(idx, &ref, now));
        ble_scheduler_note_failure(idx, now);
        ble_device_runtime_t dev = ble_test_dev(idx);
        TEST_ASSERT_EQUAL_UINT8(i + 1, dev.retry_count);
        TEST_ASSERT_EQUAL_INT64(now + expected_delays[i], dev.next_retry_ms);
        now += expected_delays[i];
    }

    ble_conn_ref_t ref;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(idx, &ref, now));
    ble_state_on_connect_success(ref, 42, now + 1);
    ble_scheduler_note_success(idx);

    ble_device_runtime_t dev = ble_test_dev(idx);
    TEST_ASSERT_EQUAL_UINT8(0, dev.retry_count);
    TEST_ASSERT_EQUAL_INT64(0, dev.next_retry_ms);
}
