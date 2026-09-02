#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ble_central.h"
#include "web_http.h"

#define BLE_SCAN_CACHE_SIZE  20
#define BLE_SCAN_DURATION_MS 6000
#define SCAN_STATE_LOCK_TIMEOUT_MS 1000

typedef struct {
    ble_scan_result_t result;
    int64_t last_seen_ms;
} scan_cache_entry_t;

static scan_cache_entry_t s_scan_cache[BLE_SCAN_CACHE_SIZE];
static int s_scan_cache_count;
static SemaphoreHandle_t s_scan_mutex;
static esp_timer_handle_t s_scan_timer;
static bool s_scan_active;
static int64_t s_scan_deadline_us;

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

static void ble_scan_timeout_callback(void *arg)
{
    (void)arg;
    bool expired = false;
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_scan_active && esp_timer_get_time() >= s_scan_deadline_us) {
            s_scan_active = false;
            expired = true;
        }
        xSemaphoreGive(s_scan_mutex);
    }
    if (expired) ble_central_scan_stop();
}

static cJSON *scan_state_response(bool success, bool scanning)
{
    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddBoolToObject(json, "success", success);
        cJSON_AddBoolToObject(json, "scanning", scanning);
    }
    return json;
}

static esp_err_t ble_scan_post_handler(httpd_req_t *request)
{
    if (ble_central_is_scanning()) {
        return web_send_json(request, scan_state_response(true, true));
    }

    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(SCAN_STATE_LOCK_TIMEOUT_MS)) !=
        pdTRUE) {
        return web_send_api_error(request, "503 Service Unavailable",
                                  "BLE scan state is temporarily unavailable");
    }
    memset(s_scan_cache, 0, sizeof(s_scan_cache));
    s_scan_cache_count = 0;
    s_scan_active = true;
    s_scan_deadline_us =
        esp_timer_get_time() + BLE_SCAN_DURATION_MS * 1000LL;
    xSemaphoreGive(s_scan_mutex);

    if (ble_central_scan_start(on_ble_scan_result) != 0) {
        if (xSemaphoreTake(s_scan_mutex,
                           pdMS_TO_TICKS(SCAN_STATE_LOCK_TIMEOUT_MS)) == pdTRUE) {
            s_scan_active = false;
            xSemaphoreGive(s_scan_mutex);
        }
        return web_send_api_error(request, "409 Conflict",
                                  "BLE host is not ready or busy");
    }

    esp_timer_stop(s_scan_timer);
    if (esp_timer_start_once(s_scan_timer,
                             BLE_SCAN_DURATION_MS * 1000ULL) != ESP_OK) {
        ble_central_scan_stop();
        if (xSemaphoreTake(s_scan_mutex,
                           pdMS_TO_TICKS(SCAN_STATE_LOCK_TIMEOUT_MS)) == pdTRUE) {
            s_scan_active = false;
            xSemaphoreGive(s_scan_mutex);
        }
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Could not schedule BLE scan timeout");
    }

    return web_send_json(request, scan_state_response(true, true));
}

static esp_err_t ble_scan_get_handler(httpd_req_t *request)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) return web_send_json(request, NULL);
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddBoolToObject(json, "scanning", ble_central_is_scanning());
    cJSON *devices = cJSON_AddArrayToObject(json, "devices");

    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(SCAN_STATE_LOCK_TIMEOUT_MS)) ==
        pdTRUE) {
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
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(SCAN_STATE_LOCK_TIMEOUT_MS)) ==
        pdTRUE) {
        s_scan_active = false;
        xSemaphoreGive(s_scan_mutex);
    }
    esp_timer_stop(s_scan_timer);

    if (ble_central_scan_stop() != 0) {
        return web_send_api_error(request, "409 Conflict",
                                  "BLE scan could not be stopped");
    }

    return web_send_json(request, scan_state_response(true, false));
}

esp_err_t web_ble_api_init(void)
{
    if (s_scan_mutex == NULL) s_scan_mutex = xSemaphoreCreateMutex();
    if (s_scan_mutex == NULL) return ESP_ERR_NO_MEM;

    if (s_scan_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = ble_scan_timeout_callback,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ble_scan_timeout",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&timer_args, &s_scan_timer) != ESP_OK) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t web_ble_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        WEB_URI_INIT("/api/ble/scan", HTTP_GET, ble_scan_get_handler),
        WEB_URI_INIT("/api/ble/scan", HTTP_POST, ble_scan_post_handler),
        WEB_URI_INIT("/api/ble/scan", HTTP_DELETE, ble_scan_delete_handler),
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
