#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "../device_store_internal.h"
#include "device_store.h"

// Reloading from NVS requires re-running the single-shot init; tests use
// the internal test-only reset hook instead of rebooting (plan §13.3).
static void reload_store(void)
{
    device_store_reset_for_test();
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_init());
}

TEST_CASE("init is single shot", "[device_store]")
{
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_INVALID_STATE, device_store_init());
    reload_store(); // Reset hook restores a runnable lifecycle.
}

TEST_CASE("adds, reads and rejects duplicates", "[device_store]")
{
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK,
                          device_store_add("plug-1", "Desk plug", "switch"));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_DUPLICATE_ID,
                          device_store_add("plug-1", "Duplicate", "switch"));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_INVALID_ARG,
                          device_store_add(NULL, "Null id", "switch"));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_INVALID_ARG,
                          device_store_add("", "Empty id", "switch"));

    char oversized[DEVICE_ID_MAX_LEN + 1];
    memset(oversized, 'a', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_INVALID_ARG,
                          device_store_add(oversized, "Too long", "switch"));

    device_entry_t entry;
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_get("plug-1", &entry));
    TEST_ASSERT_EQUAL_STRING("Desk plug", entry.name);
    TEST_ASSERT_EQUAL_STRING("switch", entry.type);
    TEST_ASSERT_FALSE(entry.has_ble_identity);

    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_NOT_FOUND,
                          device_store_get("missing", &entry));
}

TEST_CASE("edits and deletes persisted entries", "[device_store]")
{
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK,
                          device_store_add("sensor-1", "Old", "generic"));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK,
                          device_store_edit("sensor-1", "Window", "sensor"));

    device_entry_t entry;
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_get("sensor-1", &entry));
    TEST_ASSERT_EQUAL_STRING("Window", entry.name);
    TEST_ASSERT_EQUAL_STRING("sensor", entry.type);

    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_NOT_FOUND,
                          device_store_edit("missing", "New", NULL));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_INVALID_ARG,
                          device_store_edit("sensor-1", NULL, NULL));

    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_delete("sensor-1"));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_NOT_FOUND,
                          device_store_get("sensor-1", &entry));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_NOT_FOUND,
                          device_store_delete("sensor-1"));
}

TEST_CASE("persists BLE identity across re-init", "[device_store]")
{
    const uint8_t address[] = {6, 5, 4, 3, 2, 1};
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK,
                          device_store_add("lamp-1", "Lamp", "light"));
    TEST_ASSERT_EQUAL_INT(
        DEVICE_STORE_OK,
        device_store_set_ble_identity("lamp-1", address, 1));

    reload_store();

    device_entry_t entry;
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_get("lamp-1", &entry));
    TEST_ASSERT_TRUE(entry.has_ble_identity);
    TEST_ASSERT_EQUAL_UINT8(1, entry.ble_addr_type);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(address, entry.ble_addr, 6);
}

TEST_CASE("identity setter validates arguments and target", "[device_store]")
{
    const uint8_t address[] = {1, 2, 3, 4, 5, 6};
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK,
                          device_store_add("dev-a", "A", "generic"));

    TEST_ASSERT_EQUAL_INT(
        DEVICE_STORE_ERR_INVALID_ARG,
        device_store_set_ble_identity("dev-a", address,
                                      DEVICE_STORE_BLE_ADDR_TYPE_MAX + 1));
    TEST_ASSERT_EQUAL_INT(
        DEVICE_STORE_ERR_NOT_FOUND,
        device_store_set_ble_identity("missing", address, 0));

    // Same value twice is an idempotent success.
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK,
                          device_store_set_ble_identity("dev-a", address, 0));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK,
                          device_store_set_ble_identity("dev-a", address, 0));

    reload_store();
    device_entry_t entry;
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_get("dev-a", &entry));
    TEST_ASSERT_TRUE(entry.has_ble_identity);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(address, entry.ble_addr, 6);
}

TEST_CASE("rejects duplicate canonical BLE identity", "[device_store]")
{
    const uint8_t address[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const uint8_t other[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_add("d1", "D1", "generic"));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_add("d2", "D2", "generic"));

    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK,
                          device_store_set_ble_identity("d1", address, 0));
    TEST_ASSERT_EQUAL_INT(
        DEVICE_STORE_ERR_DUPLICATE_BLE_IDENTITY,
        device_store_set_ble_identity("d2", address, 0));
    // A different address type is a different canonical identity.
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK,
                          device_store_set_ble_identity("d2", address, 1));
    // Re-assigning the same identity to its owner is allowed.
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK,
                          device_store_set_ble_identity("d1", other, 0));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK,
                          device_store_set_ble_identity("d1", address, 0));
}

TEST_CASE("compacts entries after deletion across re-init", "[device_store]")
{
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_add("a", "A", "generic"));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_add("b", "B", "generic"));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_add("c", "C", "generic"));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_delete("b"));

    reload_store();

    size_t count = 0;
    device_entry_t entries[DEVICE_STORE_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(
        DEVICE_STORE_OK,
        device_store_snapshot(entries, DEVICE_STORE_MAX_DEVICES, &count));
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL_STRING("a", entries[0].device_id);
    TEST_ASSERT_EQUAL_STRING("c", entries[1].device_id);
}

TEST_CASE("snapshot never truncates silently", "[device_store]")
{
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_add("s1", "S1", "generic"));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_add("s2", "S2", "generic"));

    size_t required = 0;
    device_entry_t entries[DEVICE_STORE_MAX_DEVICES];

    // Query mode: no buffer, just the count.
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_snapshot(NULL, 0, &required));
    TEST_ASSERT_EQUAL_UINT32(2, required);

    // Too-small buffer must fail loudly and report the required size.
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_BUFFER_TOO_SMALL,
                          device_store_snapshot(entries, 1, &required));
    TEST_ASSERT_EQUAL_UINT32(2, required);

    // Invalid argument shapes.
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_INVALID_ARG,
                          device_store_snapshot(entries, DEVICE_STORE_MAX_DEVICES,
                                                NULL));
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_INVALID_ARG,
                          device_store_snapshot(NULL, 3, &required));

    TEST_ASSERT_EQUAL_INT(
        DEVICE_STORE_OK,
        device_store_snapshot(entries, DEVICE_STORE_MAX_DEVICES, &required));
    TEST_ASSERT_EQUAL_UINT32(2, required);
}

TEST_CASE("MAC-looking device id does not imply BLE identity", "[device_store]")
{
    TEST_ASSERT_EQUAL_INT(
        DEVICE_STORE_OK,
        device_store_add("11:22:33:44:55:66", "Legacy lamp", "light"));

    device_entry_t entry;
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_get("11:22:33:44:55:66", &entry));
    TEST_ASSERT_FALSE(entry.has_ble_identity);

    // Identity must arrive explicitly afterwards.
    const uint8_t address[] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    TEST_ASSERT_EQUAL_INT(
        DEVICE_STORE_OK,
        device_store_set_ble_identity("11:22:33:44:55:66", address, 0));
}

TEST_CASE("store reports full at configured capacity", "[device_store]")
{
    char id[DEVICE_ID_MAX_LEN];
    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        snprintf(id, sizeof(id), "cap-%02d", i);
        TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_add(id, id, "generic"));
    }
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_ERR_FULL,
                          device_store_add("cap-overflow", "Overflow", "generic"));
}
