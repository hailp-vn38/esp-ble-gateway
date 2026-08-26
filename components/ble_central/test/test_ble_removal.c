#include <string.h>

#include "unity.h"

#include "ble_central_internal.h"

#include "test_ble_common.h"

TEST_CASE("forget without link finalizes runtime immediately", "[ble_removal]")
{
    ble_test_bootstrap();

    uint8_t addr[6] = {1, 2, 3, 4, 5, 6};
    int idx = ble_test_register("rm-offline");

    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_central_forget_peer("rm-offline", addr, 0, false));

    TEST_ASSERT_FALSE(ble_runtime_snapshot(idx, &(ble_device_runtime_t){0}));
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_ERR_NOT_FOUND,
                          ble_runtime_find("rm-offline"));
}

TEST_CASE("forget unknown device without address is idempotent", "[ble_removal]")
{
    ble_test_bootstrap();

    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_central_forget_peer("rm-ghost", NULL, 0, false));
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_ERR_INVALID_ARG,
                          ble_central_forget_peer(NULL, NULL, 0, false));
}

TEST_CASE("disconnect during removal preserves REMOVING until finalize", "[ble_removal]")
{
    ble_test_bootstrap();

    int idx = ble_test_register("rm-linked");
    ble_conn_ref_t ref;
    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_OK,
                          ble_state_reserve_connection(idx, &ref, 1000));
    TEST_ASSERT_TRUE(ble_state_on_connect_success(ref, 11, 1100));

    TEST_ASSERT_TRUE(ble_state_lock());
    g_ble_devices[idx].state = BLE_DEVICE_REMOVING;
    g_ble_devices[idx].reconnect_enabled = false;
    ble_state_unlock();

    char id[GW_MSG_DEVICE_ID_LEN];
    bool removing = false;
    TEST_ASSERT_TRUE(ble_state_on_disconnect(
        (ble_conn_event_ref_t){.ref = ref, .conn_handle = 11}, 1200, id,
        sizeof(id), &removing, NULL, NULL));
    TEST_ASSERT_TRUE(removing);
    TEST_ASSERT_EQUAL_STRING("rm-linked", id);
    TEST_ASSERT_EQUAL_INT(-1, ble_test_dev(idx).connection_slot);
    TEST_ASSERT_EQUAL_INT(BLE_DEVICE_REMOVING, ble_test_dev(idx).state);

    ble_runtime_finalize_remove(idx);
    TEST_ASSERT_FALSE(ble_runtime_snapshot(idx, &(ble_device_runtime_t){0}));
}

TEST_CASE("removal finalize ignores entries not marked REMOVING", "[ble_removal]")
{
    ble_test_bootstrap();

    int idx = ble_test_register("rm-safe");
    ble_runtime_finalize_remove(idx);
    TEST_ASSERT_TRUE(ble_runtime_snapshot(idx, &(ble_device_runtime_t){0}));
}
