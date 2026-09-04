#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_timer.h"

#include "ble_central.h"
#include "gateway_status.h"
#include "web_http.h"
#include "wifi_prov.h"
#include "memory_policy.h"

static esp_err_t status_get_handler(httpd_req_t *request)
{
    gateway_status_t status;
    if (gateway_status_get(&status) != ESP_OK) {
        return web_send_api_error(request, "500 Internal Server Error",
                                  "Could not read gateway status");
    }

    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddNumberToObject(json, "device_count", status.device_count);
        cJSON_AddNumberToObject(json, "connected_count", status.connected_count);
        cJSON_AddNumberToObject(json, "ble_link_count", status.ble_link_count);
        cJSON_AddStringToObject(json, "ip", status.ip);
        cJSON_AddBoolToObject(json, "wifi_connected", status.wifi_connected);
        cJSON_AddBoolToObject(json, "provisioning", status.provisioning);
        cJSON_AddStringToObject(json, "wifi_state", status.wifi_state);
        cJSON_AddNumberToObject(json, "free_heap", status.free_heap);
        cJSON_AddNumberToObject(json, "uptime_ms", (double)status.uptime_ms);
        cJSON_AddStringToObject(json, "firmware_version",
                                status.firmware_version);
        cJSON_AddStringToObject(json, "idf_version", status.idf_version);
        cJSON_AddStringToObject(json, "wifi_ssid", status.wifi_ssid);
        cJSON_AddStringToObject(json, "wifi_mac", status.wifi_mac);
        if (status.has_wifi_rssi) {
            cJSON_AddNumberToObject(json, "wifi_rssi", status.wifi_rssi);
        } else {
            cJSON_AddNullToObject(json, "wifi_rssi");
        }

        // Internal SRAM telemetry
        cJSON *internal = cJSON_AddObjectToObject(json, "internal");
        if (internal != NULL) {
            cJSON_AddNumberToObject(internal, "free",
                                    (double)status.internal_free);
            cJSON_AddNumberToObject(internal, "min_free",
                                    (double)status.internal_min_free);
            cJSON_AddNumberToObject(internal, "largest_free_block",
                                    (double)status.internal_largest_free_block);
        }

        // PSRAM telemetry
        cJSON *psram = cJSON_AddObjectToObject(json, "psram");
        if (psram != NULL) {
            cJSON_AddBoolToObject(psram, "ready", status.psram_ready);
            if (status.psram_ready) {
                cJSON_AddNumberToObject(psram, "free",
                                        (double)status.psram_free);
                cJSON_AddNumberToObject(psram, "min_free",
                                        (double)status.psram_min_free);
                cJSON_AddNumberToObject(psram, "largest_free_block",
                                        (double)status.psram_largest_free_block);
            }
        }

        cJSON *memory_metrics = cJSON_AddObjectToObject(json, "memory_policy");
        if (memory_metrics != NULL) {
            cJSON_AddNumberToObject(memory_metrics, "external_alloc_success",
                                    (double)status.memory_metrics.external_alloc_success);
            cJSON_AddNumberToObject(memory_metrics, "external_alloc_fail",
                                    (double)status.memory_metrics.external_alloc_fail);
            cJSON_AddNumberToObject(memory_metrics, "internal_fallback_attempt",
                                    (double)status.memory_metrics.internal_fallback_attempt);
            cJSON_AddNumberToObject(memory_metrics, "internal_fallback_success",
                                    (double)status.memory_metrics.internal_fallback_success);
            cJSON_AddNumberToObject(memory_metrics, "internal_fallback_rejected_floor",
                                    (double)status.memory_metrics.internal_fallback_rejected_floor);
        }
        cJSON *tasks = cJSON_AddObjectToObject(json, "tasks");
        if (tasks != NULL) {
            cJSON_AddNumberToObject(tasks, "count",
                                    (double)status.task_memory_metrics.task_count);
            cJSON_AddNumberToObject(tasks, "stack_min_bytes",
                                    (double)status.task_memory_metrics.task_stack_min_bytes);
            cJSON_AddNumberToObject(tasks, "stack_unknown_count",
                                    (double)status.task_memory_metrics.task_stack_unknown_count);
            cJSON_AddNumberToObject(tasks, "system_count",
                                    (double)status.task_memory_metrics.system_task_count);
            cJSON_AddNumberToObject(tasks, "system_stack_min_bytes",
                                    (double)status.task_memory_metrics.system_stack_min_bytes);
            cJSON_AddStringToObject(tasks, "stack_min_name",
                                    status.task_memory_metrics.task_stack_min_name);
        }
        cJSON *queues = cJSON_AddObjectToObject(json, "queues");
        if (queues != NULL) {
            cJSON *ble_notify = cJSON_AddObjectToObject(queues, "ble_notify");
            if (ble_notify != NULL) {
                cJSON_AddNumberToObject(ble_notify, "high_watermark",
                                        (double)status.ble_notify_queue_high_watermark);
            }
            cJSON *schema = cJSON_AddObjectToObject(queues, "device_schema");
            if (schema != NULL) {
                cJSON_AddNumberToObject(schema, "enqueued",
                                        (double)status.schema_queue_metrics.enqueued);
                cJSON_AddNumberToObject(schema, "dropped",
                                        (double)status.schema_queue_metrics.dropped);
                cJSON_AddNumberToObject(schema, "high_watermark",
                                        (double)status.schema_queue_metrics.high_watermark);
                cJSON_AddNumberToObject(schema, "message_alloc_fail",
                                        (double)status.schema_queue_metrics.message_alloc_fail);
            }
        }

        // WebSocket transport metrics
        cJSON *ws = cJSON_AddObjectToObject(json, "websocket");
        if (ws != NULL) {
            int ws_clients = 0;
            uint32_t ws_ring_used = 0;
            bool ws_resync_pending = false;
            uint32_t ws_resync_total = 0;
            uint32_t ws_send_error_total = 0;
            uint32_t ws_connect_total = 0;
            uint32_t ws_disconnect_total = 0;
            web_event_ws_get_stats(&ws_clients, &ws_ring_used,
                                   &ws_resync_pending, &ws_resync_total,
                                   &ws_send_error_total,
                                   &ws_connect_total,
                                   &ws_disconnect_total);
            cJSON_AddNumberToObject(ws, "active_clients", ws_clients);
            cJSON_AddNumberToObject(ws, "max_clients", 2);
            cJSON_AddNumberToObject(ws, "ring_used", ws_ring_used);
            cJSON_AddNumberToObject(ws, "ring_depth", 32);
            cJSON_AddBoolToObject(ws, "resync_pending", ws_resync_pending);
            cJSON_AddNumberToObject(ws, "resync_total", ws_resync_total);
            cJSON_AddNumberToObject(ws, "send_error_total",
                                    ws_send_error_total);
            cJSON_AddNumberToObject(ws, "connect_total", ws_connect_total);
            cJSON_AddNumberToObject(ws, "disconnect_total",
                                    ws_disconnect_total);
        }
    }
    return web_send_json(request, json);
}

static esp_err_t restart_post_handler(httpd_req_t *request)
{
    if (wifi_prov_schedule_restart(1000) != 0) {
        return web_send_api_error(request, "500 Internal Server Error",
                                  "Could not schedule gateway restart");
    }

    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddStringToObject(json, "message", "Gateway restart scheduled");
    }
    return web_send_json(request, json);
}

static esp_err_t provisioning_status_get_handler(httpd_req_t *request)
{
    char ip[16];
    wifi_prov_get_ip(ip, sizeof(ip));

    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddStringToObject(json, "ip", ip);
        cJSON_AddBoolToObject(json, "wifi_connected",
                             wifi_prov_is_connected());
        cJSON_AddBoolToObject(json, "provisioning",
                             wifi_prov_is_provisioning());
        cJSON_AddStringToObject(json, "wifi_state",
                               wifi_prov_state_name(wifi_prov_get_state()));
        cJSON_AddNumberToObject(json, "free_heap", esp_get_free_heap_size());
        cJSON_AddNumberToObject(json, "uptime_ms",
                               esp_timer_get_time() / 1000);

        gw_memory_snapshot_t memory;
        gw_memory_snapshot(&memory);
        cJSON *internal = cJSON_AddObjectToObject(json, "internal");
        if (internal != NULL) {
            cJSON_AddNumberToObject(internal, "free",
                                    (double)memory.internal_free);
            cJSON_AddNumberToObject(internal, "min_free",
                                    (double)memory.internal_min_free);
            cJSON_AddNumberToObject(internal, "largest_free_block",
                                    (double)memory.internal_largest);
        }
        cJSON *psram = cJSON_AddObjectToObject(json, "psram");
        if (psram != NULL) {
            cJSON_AddBoolToObject(psram, "ready", memory.psram_ready);
            if (memory.psram_ready) {
                cJSON_AddNumberToObject(psram, "free",
                                        (double)memory.psram_free);
                cJSON_AddNumberToObject(psram, "min_free",
                                        (double)memory.psram_min_free);
                cJSON_AddNumberToObject(psram, "largest_free_block",
                                        (double)memory.psram_largest);
            }
        }
        gw_mem_metrics_t metrics;
        gw_mem_get_metrics(&metrics);
        cJSON *policy = cJSON_AddObjectToObject(json, "memory_policy");
        if (policy != NULL) {
            cJSON_AddNumberToObject(policy, "external_alloc_success",
                                    (double)metrics.external_alloc_success);
            cJSON_AddNumberToObject(policy, "external_alloc_fail",
                                    (double)metrics.external_alloc_fail);
            cJSON_AddNumberToObject(policy, "internal_fallback_attempt",
                                    (double)metrics.internal_fallback_attempt);
            cJSON_AddNumberToObject(policy, "internal_fallback_success",
                                    (double)metrics.internal_fallback_success);
            cJSON_AddNumberToObject(policy, "internal_fallback_rejected_floor",
                                    (double)metrics.internal_fallback_rejected_floor);
        }
        gw_task_memory_metrics_t task_metrics;
        gw_memory_task_snapshot(&task_metrics);
        cJSON *tasks = cJSON_AddObjectToObject(json, "tasks");
        if (tasks != NULL) {
            cJSON_AddNumberToObject(tasks, "count", (double)task_metrics.task_count);
            cJSON_AddNumberToObject(tasks, "stack_min_bytes",
                                    (double)task_metrics.task_stack_min_bytes);
            cJSON_AddNumberToObject(tasks, "stack_unknown_count",
                                    (double)task_metrics.task_stack_unknown_count);
            cJSON_AddNumberToObject(tasks, "system_count",
                                    (double)task_metrics.system_task_count);
            cJSON_AddNumberToObject(tasks, "system_stack_min_bytes",
                                    (double)task_metrics.system_stack_min_bytes);
            cJSON_AddStringToObject(tasks, "stack_min_name",
                                    task_metrics.task_stack_min_name);
        }
        cJSON *queues = cJSON_AddObjectToObject(json, "queues");
        if (queues != NULL) {
            cJSON *ble_notify = cJSON_AddObjectToObject(queues, "ble_notify");
            if (ble_notify != NULL) {
                cJSON_AddNumberToObject(ble_notify, "high_watermark",
                                        (double)ble_central_notify_queue_high_watermark());
            }
        }
    }
    return web_send_json(request, json);
}

esp_err_t web_system_api_register_gateway(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        WEB_URI_INIT("/api/status", HTTP_GET, status_get_handler),
        WEB_URI_INIT("/api/restart", HTTP_POST, restart_post_handler),
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}

esp_err_t web_system_api_register_provisioning(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        WEB_URI_INIT("/api/status", HTTP_GET, provisioning_status_get_handler),
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
