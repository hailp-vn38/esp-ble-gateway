#include <string.h>

#include "unity.h"

#include "ble_central_internal.h"

#include "test_ble_common.h"

TEST_CASE("runtime find or register is idempotent and refreshes address", "[ble_runtime]")
{
    ble_test_bootstrap();

    ble_addr_t addr = {.type = 0, .val = {1, 2, 3, 4, 5, 6}};
    int first = ble_test_register("dev-idem");
    int second = ble_runtime_find_or_register("dev-idem", &addr);
    TEST_ASSERT_EQUAL_INT(first, second);

    TEST_ASSERT_TRUE(ble_runtime_get_peer_addr(first, &addr));
    TEST_ASSERT_EQUAL_UINT8(6, addr.val[5]);

    TEST_ASSERT_EQUAL_INT(BLE_CENTRAL_ERR_NOT_FOUND,
                          ble_runtime_find("dev-missing"));
}

TEST_CASE("runtime removal does not compact indices", "[ble_runtime]")
{
    ble_test_bootstrap();

    int a = ble_test_register("dev-a");
    int b = ble_test_register("dev-b");
    int c = ble_test_register("dev-c");

    TEST_ASSERT_TRUE(ble_state_lock());
    g_ble_devices[b].state = BLE_DEVICE_REMOVING;
    ble_state_unlock();

    ble_runtime_finalize_remove(b);

    char id[GW_MSG_DEVICE_ID_LEN];
    TEST_ASSERT_TRUE(ble_runtime_get_device_id(a, id, sizeof(id)));
    TEST_ASSERT_EQUAL_STRING("dev-a", id);
    TEST_ASSERT_TRUE(ble_runtime_get_device_id(c, id, sizeof(id)));
    TEST_ASSERT_EQUAL_STRING("dev-c", id);
    TEST_ASSERT_FALSE(ble_runtime_snapshot(b, &(ble_device_runtime_t){0}));
}

TEST_CASE("removing entry is not reused by new registration", "[ble_runtime]")
{
    ble_test_bootstrap();

    int a = ble_test_register("dev-a");
    int b = ble_test_register("dev-b");

    TEST_ASSERT_TRUE(ble_state_lock());
    g_ble_devices[b].state = BLE_DEVICE_REMOVING;
    ble_state_unlock();

    int fresh = ble_runtime_find_or_register("dev-fresh", NULL);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fresh);
    TEST_ASSERT_NOT_EQUAL(b, fresh);

    ble_runtime_finalize_remove(b);
    int recycled = ble_runtime_find_or_register("dev-recycled", NULL);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, recycled);
    (void)a;
}
