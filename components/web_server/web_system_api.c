#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "command_executor.h"
#include "gateway_status.h"
#include "log_buffer.h"
#include "web_http.h"
#include "wifi_prov.h"

#define LOG_API_MAX_ENTRIES LOG_BUFFER_CAPACITY

static log_entry_t s_log_snapshot[LOG_API_MAX_ENTRIES];
static SemaphoreHandle_t s_log_mutex;

static esp_err_t ensure_resources(void)
{
    if (s_log_mutex == NULL) s_log_mutex = xSemaphoreCreateMutex();
    return s_log_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t logs_get_handler(httpd_req_t *request)
{
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Log buffer is busy");
    }

    int count = log_buffer_get_recent(s_log_snapshot, LOG_API_MAX_ENTRIES);
    cJSON *array = NULL;
    if (count >= 0) {
        array = cJSON_CreateArray();
        for (int i = 0; array != NULL && i < count; i++) {
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) {
                cJSON_Delete(array);
                array = NULL;
                break;
            }
            cJSON_AddStringToObject(item, "text", s_log_snapshot[i].text);
            cJSON_AddNumberToObject(item, "timestamp_ms",
                                    s_log_snapshot[i].uptime_ms);
            cJSON_AddItemToArray(array, item);
        }
    }
    xSemaphoreGive(s_log_mutex);

    if (count < 0) {
        return web_send_api_error(request, "500 Internal Server Error",
                                  "Could not read logs");
    }
    return web_send_json(request, array);
}

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

        // Executor runtime metrics + worker stack headroom (Plan v2 §53-§55).
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
    esp_err_t init_error = ensure_resources();
    if (init_error != ESP_OK) return init_error;

    static const httpd_uri_t routes[] = {
        {.uri = "/api/logs", .method = HTTP_GET,
         .handler = logs_get_handler},
        {.uri = "/api/status", .method = HTTP_GET,
         .handler = status_get_handler},
        {.uri = "/api/restart", .method = HTTP_POST,
         .handler = restart_post_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}

esp_err_t web_system_api_register_provisioning(httpd_handle_t server)
{
    esp_err_t init_error = ensure_resources();
    if (init_error != ESP_OK) return init_error;

    static const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET,
         .handler = provisioning_status_get_handler},
        {.uri = "/api/logs", .method = HTTP_GET,
         .handler = logs_get_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
