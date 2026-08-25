#include "web_modules.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ble_central.h"
#include "web_http.h"

#define BLE_SCAN_CACHE_SIZE  20
#define BLE_SCAN_DURATION_MS 6000

typedef struct {
    ble_scan_result_t result;
    int64_t last_seen_ms;
} scan_cache_entry_t;

static scan_cache_entry_t s_scan_cache[BLE_SCAN_CACHE_SIZE];
static int s_scan_cache_count;
static SemaphoreHandle_t s_scan_mutex;
static TaskHandle_t s_scan_stop_task;

static void format_ble_addr(const uint8_t address[6], char output[18])
{
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[5], address[4], address[3], address[2], address[1],
             address[0]);
}

static void on_ble_scan_result(const ble_scan_result_t *result)
{
    if (result == NULL || s_scan_mutex == NULL ||
        xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    int index = -1;
    for (int i = 0; i < s_scan_cache_count; i++) {
        if (s_scan_cache[i].result.addr_type == result->addr_type &&
            memcmp(s_scan_cache[i].result.addr, result->addr,
                   sizeof(result->addr)) == 0) {
            index = i;
            break;
        }
    }
    if (index < 0 && s_scan_cache_count < BLE_SCAN_CACHE_SIZE) {
        index = s_scan_cache_count++;
    }
    if (index >= 0) {
        s_scan_cache[index].result = *result;
        s_scan_cache[index].last_seen_ms = esp_timer_get_time() / 1000;
    }
    xSemaphoreGive(s_scan_mutex);
}

static void ble_scan_stop_worker(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(BLE_SCAN_DURATION_MS));
    ble_central_scan_stop();

    if (xSemaphoreTake(s_scan_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_scan_stop_task == xTaskGetCurrentTaskHandle()) {
            s_scan_stop_task = NULL;
        }
        xSemaphoreGive(s_scan_mutex);
    }
    vTaskDelete(NULL);
}

static TaskHandle_t detach_stop_task(void)
{
    TaskHandle_t task = s_scan_stop_task;
    s_scan_stop_task = NULL;
    return task;
}

static esp_err_t ble_scan_post_handler(httpd_req_t *request)
{
    if (ble_central_is_scanning()) {
        cJSON *json = cJSON_CreateObject();
        if (json != NULL) {
            cJSON_AddBoolToObject(json, "success", true);
            cJSON_AddBoolToObject(json, "scanning", true);
        }
        return web_send_json(request, json);
    }

    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return web_send_api_error(request, "503 Service Unavailable",
                                  "BLE scan state is temporarily unavailable");
    }
    TaskHandle_t stale_stop_task = detach_stop_task();
    memset(s_scan_cache, 0, sizeof(s_scan_cache));
    s_scan_cache_count = 0;
    xSemaphoreGive(s_scan_mutex);
    if (stale_stop_task != NULL) vTaskDelete(stale_stop_task);

    if (ble_central_scan_start(on_ble_scan_result) != 0) {
        return web_send_api_error(request, "409 Conflict",
                                  "BLE host is not ready or busy");
    }

    TaskHandle_t stop_task = NULL;
    if (xTaskCreate(ble_scan_stop_worker, "ble_scan_stop", 2048, NULL, 4,
                    &stop_task) != pdPASS) {
        ble_central_scan_stop();
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Could not schedule BLE scan timeout");
    }
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vTaskDelete(stop_task);
        ble_central_scan_stop();
        return web_send_api_error(request, "503 Service Unavailable",
                                  "BLE scan state is temporarily unavailable");
    }
    s_scan_stop_task = stop_task;
    xSemaphoreGive(s_scan_mutex);

    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddBoolToObject(json, "scanning", true);
    }
    return web_send_json(request, json);
}

static esp_err_t ble_scan_get_handler(httpd_req_t *request)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) return web_send_json(request, NULL);
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddBoolToObject(json, "scanning", ble_central_is_scanning());
    cJSON *devices = cJSON_AddArrayToObject(json, "devices");

    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        for (int i = 0; devices != NULL && i < s_scan_cache_count; i++) {
            char address[18];
            format_ble_addr(s_scan_cache[i].result.addr, address);
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) break;
            cJSON_AddStringToObject(item, "name", s_scan_cache[i].result.name);
            cJSON_AddStringToObject(item, "ble_addr", address);
            cJSON_AddNumberToObject(item, "addr_type",
                                   s_scan_cache[i].result.addr_type);
            cJSON_AddNumberToObject(item, "rssi", s_scan_cache[i].result.rssi);
            cJSON_AddItemToArray(devices, item);
        }
        xSemaphoreGive(s_scan_mutex);
    }
    return web_send_json(request, json);
}

static esp_err_t ble_scan_delete_handler(httpd_req_t *request)
{
    if (ble_central_scan_stop() != 0) {
        return web_send_api_error(request, "409 Conflict",
                                  "BLE scan could not be stopped");
    }

    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        TaskHandle_t stop_task = detach_stop_task();
        xSemaphoreGive(s_scan_mutex);
        if (stop_task != NULL) vTaskDelete(stop_task);
    }

    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddBoolToObject(json, "scanning", false);
    }
    return web_send_json(request, json);
}

esp_err_t web_ble_api_init(void)
{
    if (s_scan_mutex == NULL) s_scan_mutex = xSemaphoreCreateMutex();
    return s_scan_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t web_ble_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/ble/scan", .method = HTTP_GET,
         .handler = ble_scan_get_handler},
        {.uri = "/api/ble/scan", .method = HTTP_POST,
         .handler = ble_scan_post_handler},
        {.uri = "/api/ble/scan", .method = HTTP_DELETE,
         .handler = ble_scan_delete_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
