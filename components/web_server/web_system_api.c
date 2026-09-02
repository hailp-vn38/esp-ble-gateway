#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_timer.h"

#include "command_executor.h"
#include "gateway_status.h"
#include "web_http.h"
#include "wifi_prov.h"

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

        // Executor runtime metrics + worker stack headroom.
        command_executor_stats_t executor;
        uint32_t worker_stack_min = 0;
        command_executor_get_stats(&executor, &worker_stack_min);
        cJSON *metrics = cJSON_AddObjectToObject(json, "executor");
        if (metrics != NULL) {
            cJSON_AddNumberToObject(metrics, "submitted",
                                    (double)executor.submitted);
            cJSON_AddNumberToObject(metrics, "completed",
                                    (double)executor.completed);
            cJSON_AddNumberToObject(metrics, "queue_full",
                                    (double)executor.queue_full);
            cJSON_AddNumberToObject(metrics, "queue_timeout",
                                    (double)executor.queue_timeout);
            cJSON_AddNumberToObject(metrics, "dispatch_timeout",
                                    (double)executor.dispatch_timeout);
            cJSON_AddNumberToObject(metrics, "max_queue_depth",
                                    (double)executor.max_queue_depth);
            cJSON_AddNumberToObject(metrics, "max_queue_wait_ms",
                                    (double)executor.max_queue_wait_ms);
            cJSON_AddNumberToObject(metrics, "worker_stack_min_bytes",
                                    (double)worker_stack_min);
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
