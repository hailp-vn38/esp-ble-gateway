#ifndef BLE_TEST_COMMON_H
#define BLE_TEST_COMMON_H

#include <string.h>

#include "unity.h"

#include "ble_central_internal.h"

static void ble_test_bootstrap(void)
{
    static bool initialized;
    if (!initialized) {
        TEST_ASSERT_EQUAL_INT(0, ble_central_state_init(NULL));
        initialized = true;
    }

    TEST_ASSERT_EQUAL_UINT32(0, ble_state_handle_host_reset(NULL, 0));

    TEST_ASSERT_TRUE(ble_state_lock());
    memset(g_ble_devices, 0, sizeof(g_ble_devices));
    ble_state_unlock();

    ble_host_set_ready(true);
}

static inline int ble_test_register(const char *device_id)
{
    int idx = ble_runtime_find_or_register(device_id, NULL);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, idx);
    return idx;
}

static inline uint32_t ble_test_generation_of(int slot_index)
{
    TEST_ASSERT_TRUE(ble_state_lock());
    uint32_t generation = g_ble_connections[slot_index].generation;
    ble_state_unlock();
    return generation;
}

static inline ble_device_runtime_t ble_test_dev(int device_index)
{
    ble_device_runtime_t snapshot;
    TEST_ASSERT_TRUE(ble_runtime_snapshot(device_index, &snapshot));
    return snapshot;
}

#endif /* BLE_TEST_COMMON_H */
