#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ble_central.h"
#include "device_store.h"
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
                                    s_log_snapshot[i].timestamp_ms);
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
    device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
    int count = device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES);
    if (count < 0) count = 0;

    int connected = 0;
    for (int i = 0; i < count; i++) connected += devices[i].connected != 0;

    char ip[16];
    wifi_prov_get_ip(ip, sizeof(ip));

    wifi_ap_record_t access_point = {0};
    bool has_access_point = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;

    uint8_t mac[6] = {0};
    char mac_text[18] = "00:00:00:00:00:00";
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(mac_text, sizeof(mac_text),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddNumberToObject(json, "device_count", count);
        cJSON_AddNumberToObject(json, "connected_count", connected);
        cJSON_AddNumberToObject(json, "ble_link_count",
                               ble_central_active_count());
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
        cJSON_AddStringToObject(json, "firmware_version", app->version);
        cJSON_AddStringToObject(json, "idf_version", app->idf_ver);
        cJSON_AddStringToObject(
            json, "wifi_ssid",
            has_access_point ? (const char *)access_point.ssid : "");
        cJSON_AddStringToObject(json, "wifi_mac", mac_text);
        if (has_access_point) {
            cJSON_AddNumberToObject(json, "wifi_rssi", access_point.rssi);
        } else {
            cJSON_AddNullToObject(json, "wifi_rssi");
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
