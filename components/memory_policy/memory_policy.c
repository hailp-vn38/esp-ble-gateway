#include "memory_policy.h"

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "sdkconfig.h"

#ifdef CONFIG_SPIRAM
static bool can_fallback_internal(size_t size)
{
    if (size > CONFIG_GW_MEM_FALLBACK_MAX_BYTES) {
        return false;
    }

    size_t free_internal =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (free_internal <= size) {
        return false;
    }

    if ((free_internal - size) < CONFIG_GW_MEM_INTERNAL_FLOOR_BYTES) {
        return false;
    }

    return largest_internal >= CONFIG_GW_MEM_INTERNAL_LARGEST_FLOOR_BYTES;
}
#endif

void *gw_mem_alloc(size_t size, gw_mem_class_t mem_class)
{
    if (size == 0) {
        return NULL;
    }

    switch (mem_class) {
    case GW_MEM_INTERNAL_REQUIRED:
        return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    case GW_MEM_EXTERNAL_REQUIRED:
#ifdef CONFIG_SPIRAM
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        return NULL;
#endif

    case GW_MEM_EXTERNAL_PREFERRED:
#ifdef CONFIG_SPIRAM
        if (esp_psram_is_initialized()) {
            void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p != NULL) {
                return p;
            }
            if (!can_fallback_internal(size)) {
                return NULL;
            }
        }
#endif
        return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    case GW_MEM_DEFAULT:
    default:
        return malloc(size);
    }
}

void *gw_mem_calloc(size_t count, size_t size, gw_mem_class_t mem_class)
{
    if (count == 0 || size == 0) {
        return NULL;
    }

    size_t total = count * size;
    if (total / size != count) {
        return NULL;
    }

    void *p = gw_mem_alloc(total, mem_class);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

void gw_mem_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    free(ptr);
}

esp_err_t gw_memory_verify_psram(void)
{
#ifdef CONFIG_SPIRAM
    if (!esp_psram_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t free_psram = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (free_psram == 0) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
