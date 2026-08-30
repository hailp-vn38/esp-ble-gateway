#ifndef MEMORY_POLICY_H
#define MEMORY_POLICY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_MEM_INTERNAL_REQUIRED = 0,
    GW_MEM_EXTERNAL_REQUIRED,
    GW_MEM_EXTERNAL_PREFERRED,
    GW_MEM_DEFAULT,
} gw_mem_class_t;

void *gw_mem_alloc(size_t size, gw_mem_class_t mem_class);
void *gw_mem_calloc(size_t count, size_t size, gw_mem_class_t mem_class);
void gw_mem_free(void *ptr);

esp_err_t gw_memory_verify_psram(void);

/* ── Shared memory telemetry ──────────────────────────────────────────
 * Single source of truth for heap metrics (Plan v1.1 §8). Consumers
 * (gateway_status, perf metrics, diagnostics) must use this helper
 * instead of querying heap_caps directly. */
typedef struct {
    size_t internal_free;
    size_t internal_min_free;
    size_t internal_largest;

    bool psram_ready;
    size_t psram_free;
    size_t psram_min_free;
    size_t psram_largest;
} gw_memory_snapshot_t;

void gw_memory_snapshot(gw_memory_snapshot_t *out);

/* ── Allocation policy metrics (Plan v1.1 §15.4) ───────────────────── */
typedef struct {
    uint32_t external_alloc_success;
    uint32_t external_alloc_fail;
    uint32_t internal_fallback_attempt;
    uint32_t internal_fallback_success;
    uint32_t internal_fallback_rejected_floor;
} gw_mem_metrics_t;

void gw_mem_get_metrics(gw_mem_metrics_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_POLICY_H */
