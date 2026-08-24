#include "nvs_flash.h"
#include "unity.h"

#include "device_store.h"
#include "log_buffer.h"

void setUp(void)
{
    device_entry_t entries[DEVICE_STORE_MAX_DEVICES];
    int count = device_store_snapshot(entries, DEVICE_STORE_MAX_DEVICES);
    for (int i = 0; i < count; i++) device_store_delete(entries[i].device_id);
    log_buffer_init();
}

void tearDown(void)
{
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
    TEST_ASSERT_EQUAL_INT(0, device_store_init());

    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
    unity_run_menu();
}
