#include "memory_policy.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "sdkconfig.h"

#ifdef CONFIG_SPIRAM
static gw_mem_metrics_t s_metrics;

static inline void metrics_add(uint32_t *field)
{
    __atomic_fetch_add(field, 1, __ATOMIC_RELAXED);
}

/* Preflight: reject fallbacks that are guaranteed to fail or to violate the
 * floors, so we do not churn the internal heap for allocations that cannot
 * succeed (Plan v1.1 §15.3). */
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

/* Post-allocation enforcement (Plan v1.1 §15.2): the preflight estimate can
 * be stale under concurrency, and fragmentation means the block is not
 * necessarily carved out of the largest block. Verify the actual floors
 * after the allocation and undo it if they are violated. */
static bool floors_held_after_internal_alloc(void)
{
    size_t free_after =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_after =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    return free_after >= CONFIG_GW_MEM_INTERNAL_FLOOR_BYTES &&
           largest_after >= CONFIG_GW_MEM_INTERNAL_LARGEST_FLOOR_BYTES;
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
                metrics_add(&s_metrics.external_alloc_success);
                return p;
            }
            metrics_add(&s_metrics.external_alloc_fail);
            if (!can_fallback_internal(size)) {
                return NULL;
            }
            metrics_add(&s_metrics.internal_fallback_attempt);
            p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (p == NULL) {
                return NULL;
            }
            if (!floors_held_after_internal_alloc()) {
                free(p);
                metrics_add(&s_metrics.internal_fallback_rejected_floor);
                return NULL;
            }
            metrics_add(&s_metrics.internal_fallback_success);
            return p;
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

void gw_memory_snapshot(gw_memory_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out->internal_min_free =
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out->internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

#ifdef CONFIG_SPIRAM
    out->psram_ready = esp_psram_is_initialized();
    if (out->psram_ready) {
        out->psram_free =
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        out->psram_min_free =
            heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        out->psram_largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#endif
}

void gw_mem_get_metrics(gw_mem_metrics_t *out)
{
    if (out == NULL) {
        return;
    }
#ifdef CONFIG_SPIRAM
    out->external_alloc_success =
        __atomic_load_n(&s_metrics.external_alloc_success, __ATOMIC_RELAXED);
    out->external_alloc_fail =
        __atomic_load_n(&s_metrics.external_alloc_fail, __ATOMIC_RELAXED);
    out->internal_fallback_attempt =
        __atomic_load_n(&s_metrics.internal_fallback_attempt, __ATOMIC_RELAXED);
    out->internal_fallback_success =
        __atomic_load_n(&s_metrics.internal_fallback_success, __ATOMIC_RELAXED);
    out->internal_fallback_rejected_floor =
        __atomic_load_n(&s_metrics.internal_fallback_rejected_floor,
                        __ATOMIC_RELAXED);
#else
    memset(out, 0, sizeof(*out));
#endif
}
