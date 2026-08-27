#include "nvs_flash.h"
#include "unity.h"

#include "device_store.h"

void setUp(void)
{
    device_entry_t entries[DEVICE_STORE_MAX_DEVICES];
    size_t count = 0;
    if (device_store_snapshot(entries, DEVICE_STORE_MAX_DEVICES, &count) ==
        DEVICE_STORE_OK) {
        for (size_t i = 0; i < count; i++) {
            device_store_delete(entries[i].device_id);
        }
    }
}

void tearDown(void)
{
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
    TEST_ASSERT_EQUAL_INT(DEVICE_STORE_OK, device_store_init());

    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
    unity_run_menu();
}
