#include <string.h>

#include "unity.h"

#include "device_store.h"

TEST_CASE("device store adds, finds and rejects duplicates", "[device_store]")
{
    TEST_ASSERT_EQUAL_INT(0, device_store_add("plug-1", "Desk plug", "switch"));
    TEST_ASSERT_EQUAL_INT(-1, device_store_add("plug-1", "Duplicate", "switch"));

    device_entry_t entry;
    TEST_ASSERT_EQUAL_INT(0, device_store_get("plug-1", &entry));
    TEST_ASSERT_EQUAL_STRING("Desk plug", entry.name);
    TEST_ASSERT_EQUAL_STRING("switch", entry.type);
}

TEST_CASE("device store edits and deletes persisted entries", "[device_store]")
{
    TEST_ASSERT_EQUAL_INT(0, device_store_add("sensor-1", "Old", "generic"));
    TEST_ASSERT_EQUAL_INT(0, device_store_edit("sensor-1", "Window", "sensor"));

    device_entry_t entry;
    TEST_ASSERT_EQUAL_INT(0, device_store_get("sensor-1", &entry));
    TEST_ASSERT_EQUAL_STRING("Window", entry.name);
    TEST_ASSERT_EQUAL_STRING("sensor", entry.type);
    TEST_ASSERT_EQUAL_INT(0, device_store_delete("sensor-1"));
    TEST_ASSERT_EQUAL_INT(-1, device_store_get("sensor-1", &entry));
}

TEST_CASE("device store persists BLE address across re-init", "[device_store]")
{
    const uint8_t address[] = {6, 5, 4, 3, 2, 1};
    TEST_ASSERT_EQUAL_INT(0, device_store_add("lamp-1", "Lamp", "light"));
    TEST_ASSERT_EQUAL_INT(0, device_store_set_ble_addr("lamp-1", address, 1));
    TEST_ASSERT_EQUAL_INT(0, device_store_init());

    device_entry_t entry;
    TEST_ASSERT_EQUAL_INT(0, device_store_get("lamp-1", &entry));
    TEST_ASSERT_TRUE(entry.has_ble_addr);
    TEST_ASSERT_EQUAL_UINT8(1, entry.ble_addr_type);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(address, entry.ble_addr, 6);
}

TEST_CASE("device store compacts entries after deletion", "[device_store]")
{
    TEST_ASSERT_EQUAL_INT(0, device_store_add("a", "A", "generic"));
    TEST_ASSERT_EQUAL_INT(0, device_store_add("b", "B", "generic"));
    TEST_ASSERT_EQUAL_INT(0, device_store_add("c", "C", "generic"));
    TEST_ASSERT_EQUAL_INT(0, device_store_delete("b"));
    TEST_ASSERT_EQUAL_INT(0, device_store_init());

    device_entry_t entries[DEVICE_STORE_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(2, device_store_snapshot(entries, DEVICE_STORE_MAX_DEVICES));
    TEST_ASSERT_EQUAL_STRING("a", entries[0].device_id);
    TEST_ASSERT_EQUAL_STRING("c", entries[1].device_id);
}

TEST_CASE("MAC device id is migrated into NimBLE address order", "[device_store]")
{
    TEST_ASSERT_EQUAL_INT(
        0, device_store_add("11:22:33:44:55:66", "Legacy lamp", "light"));
    device_entry_t entry;
    TEST_ASSERT_EQUAL_INT(0, device_store_get("11:22:33:44:55:66", &entry));
    TEST_ASSERT_TRUE(entry.has_ble_addr);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        ((uint8_t[]){0x66, 0x55, 0x44, 0x33, 0x22, 0x11}),
        entry.ble_addr, 6);
}
