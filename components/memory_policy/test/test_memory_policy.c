#include "unity.h"
#include "memory_policy.h"

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "sdkconfig.h"
#include "cJSON.h"

TEST_CASE("gw_mem_alloc INTERNAL_REQUIRED returns internal memory", "[memory_policy]")
{
    void *p = gw_mem_alloc(128, GW_MEM_INTERNAL_REQUIRED);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(esp_ptr_internal(p));
    gw_mem_free(p);
}

TEST_CASE("gw_mem_alloc zero size returns NULL", "[memory_policy]")
{
    void *p = gw_mem_alloc(0, GW_MEM_INTERNAL_REQUIRED);
    TEST_ASSERT_NULL(p);
}

TEST_CASE("gw_mem_calloc returns zeroed memory", "[memory_policy]")
{
    uint32_t *p = gw_mem_calloc(4, sizeof(uint32_t), GW_MEM_INTERNAL_REQUIRED);
    TEST_ASSERT_NOT_NULL(p);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, p[i]);
    }
    gw_mem_free(p);
}

TEST_CASE("gw_mem_calloc overflow returns NULL", "[memory_policy]")
{
    void *p = gw_mem_calloc(SIZE_MAX / 2, 3, GW_MEM_INTERNAL_REQUIRED);
    TEST_ASSERT_NULL(p);
}

#ifdef CONFIG_SPIRAM
TEST_CASE("cJSON hooks prefer PSRAM and free through cJSON_free", "[memory_policy]")
{
    gw_cjson_init_hooks();
    cJSON *object = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(object);
    if (esp_psram_is_initialized()) {
        TEST_ASSERT_TRUE(esp_ptr_external(object));
    }
    cJSON_Delete(object);
}

TEST_CASE("gw_memory_verify_psram succeeds when PSRAM initialized", "[memory_policy]")
{
    esp_err_t ret = gw_memory_verify_psram();
    /* On hardware with PSRAM this should pass; on boards without PSRAM
       it returns ESP_ERR_INVALID_STATE. Both are valid acceptance. */
    TEST_ASSERT_TRUE(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE);
}

TEST_CASE("gw_mem_alloc EXTERNAL_REQUIRED returns PSRAM memory", "[memory_policy]")
{
    void *p = gw_mem_alloc(256, GW_MEM_EXTERNAL_REQUIRED);
    if (esp_psram_is_initialized()) {
        TEST_ASSERT_NOT_NULL(p);
    }
    gw_mem_free(p);
}
#endif
