#ifndef GATEWAY_STATUS_H
#define GATEWAY_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "memory_policy.h"
#include "device_schema.h"

// Aggregated gateway snapshot. Single source of truth for
// REST /api/status, the dispatcher get_status command, and MCP.
typedef struct {
    int device_count;
    int connected_count;
    int ble_link_count;

    char ip[16];
    bool wifi_connected;
    bool provisioning;
    char wifi_state[24];

    uint32_t free_heap;
    uint64_t uptime_ms;

    char firmware_version[32];
    char idf_version[32];

    char wifi_ssid[33];
    char wifi_mac[18];
    bool has_wifi_rssi;
    int wifi_rssi;

    // Internal SRAM telemetry (Phase P4)
    uint32_t internal_free;
    uint32_t internal_min_free;
    uint32_t internal_largest_free_block;

    // PSRAM telemetry (Phase P4)
    bool psram_ready;
    uint32_t psram_free;
    uint32_t psram_min_free;
    uint32_t psram_largest_free_block;

    // Allocation-policy counters since boot.
    gw_mem_metrics_t memory_metrics;
    gw_task_memory_metrics_t task_memory_metrics;
    uint32_t ble_notify_queue_high_watermark;
    device_schema_queue_stats_t schema_queue_metrics;
} gateway_status_t;

// Fills status with a consistent point-in-time snapshot. Never blocks on
// BLE; safe to call from HTTPD tasks and dispatcher handlers.
esp_err_t gateway_status_get(gateway_status_t *status);

#endif // GATEWAY_STATUS_H
