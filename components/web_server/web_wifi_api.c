#include "web_modules.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "web_http.h"
#include "wifi_prov.h"

#define WIFI_SCAN_WORKER_STACK   8192
#define WIFI_CONFIG_WORKER_STACK 4096
#define WIFI_RESTART_DELAY_MS    4000
#define WIFI_JOB_MESSAGE_LEN     128

static const char *TAG = "web_wifi_api";

typedef enum {
    WIFI_JOB_IDLE = 0,
    WIFI_JOB_CONNECTING,
    WIFI_JOB_SUCCEEDED,
    WIFI_JOB_FAILED,
} wifi_job_state_t;

typedef struct {
    char ssid[33];
    char password[65];
} wifi_config_context_t;

static SemaphoreHandle_t s_wifi_mutex;
static wifi_prov_ap_record_t s_scan_cache[WIFI_PROV_MAX_SCAN_RESULTS];
static size_t s_scan_count;
static bool s_scan_running;
static int s_scan_result;
static wifi_job_state_t s_job_state = WIFI_JOB_IDLE;
static char s_job_message[WIFI_JOB_MESSAGE_LEN] = "Ready";

static const char *wifi_job_state_name(wifi_job_state_t state)
{
    switch (state) {
    case WIFI_JOB_CONNECTING: return "connecting";
    case WIFI_JOB_SUCCEEDED: return "succeeded";
    case WIFI_JOB_FAILED: return "failed";
    default: return "idle";
    }
}

static const char *wifi_scan_error_message(int result)
{
    switch (result) {
    case -2: return "Gateway is not provisioning";
    case -3: return "Another Wi-Fi operation is in progress";
    default: return "Wi-Fi scan failed";
    }
}

static const char *wifi_config_error_message(int result)
{
    switch (result) {
    case -2: return "Gateway is not provisioning";
    case -3: return "Another Wi-Fi operation is in progress";
    case -4: return "Could not start the Wi-Fi connection";
    case -5: return "Connection failed; check SSID and password";
    case -6: return "Connected, but credentials could not be saved";
    default: return "Could not verify Wi-Fi configuration";
    }
}

static void free_wifi_context(wifi_config_context_t *context)
{
    if (context == NULL) return;
    memset(context->password, 0, sizeof(context->password));
    free(context);
}

static void wifi_scan_worker(void *arg)
{
    (void)arg;
    wifi_prov_ap_record_t records[WIFI_PROV_MAX_SCAN_RESULTS] = {0};
    size_t count = 0;
    int64_t started_us = esp_timer_get_time();
    int result = wifi_prov_scan(records, WIFI_PROV_MAX_SCAN_RESULTS, &count);
    int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;

    if (count > WIFI_PROV_MAX_SCAN_RESULTS) count = WIFI_PROV_MAX_SCAN_RESULTS;
    if (xSemaphoreTake(s_wifi_mutex, portMAX_DELAY) == pdTRUE) {
        if (result == 0) {
            memcpy(s_scan_cache, records, count * sizeof(records[0]));
            s_scan_count = count;
        }
        s_scan_result = result;
        s_scan_running = false;
        xSemaphoreGive(s_wifi_mutex);
    }

    if (result == 0) {
        ESP_LOGI(TAG, "Wi-Fi scan completed with %u networks in %lld ms",
                 (unsigned)count, (long long)elapsed_ms);
    } else {
        ESP_LOGW(TAG, "Wi-Fi scan failed in %lld ms: %d",
                 (long long)elapsed_ms, result);
    }
    vTaskDelete(NULL);
}

static esp_err_t wifi_scan_post_handler(httpd_req_t *request)
{
    if (!wifi_prov_is_provisioning()) {
        return web_send_api_error(request, "409 Conflict",
                                  "Gateway is not provisioning");
    }
    if (s_wifi_mutex == NULL ||
        xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Wi-Fi state is temporarily unavailable");
    }
    if (s_job_state == WIFI_JOB_CONNECTING) {
        xSemaphoreGive(s_wifi_mutex);
        return web_send_api_error(request, "409 Conflict",
                                  "Wi-Fi configuration is in progress");
    }

    bool already_running = s_scan_running;
    if (!already_running) {
        s_scan_running = true;
        s_scan_result = 0;
    }
    xSemaphoreGive(s_wifi_mutex);

    if (!already_running &&
        xTaskCreate(wifi_scan_worker, "wifi_scan", WIFI_SCAN_WORKER_STACK,
                    NULL, 4, NULL) != pdPASS) {
        if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_scan_running = false;
            s_scan_result = -1;
            xSemaphoreGive(s_wifi_mutex);
        }
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Could not start Wi-Fi scan worker");
    }

    httpd_resp_set_status(request, "202 Accepted");
    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddBoolToObject(json, "scanning", true);
    }
    return web_send_json(request, json);
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *request)
{
    wifi_prov_ap_record_t records[WIFI_PROV_MAX_SCAN_RESULTS] = {0};
    size_t count;
    bool scanning;
    int result;

    if (s_wifi_mutex == NULL ||
        xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Wi-Fi state is temporarily unavailable");
    }
    scanning = s_scan_running;
    result = s_scan_result;
    count = s_scan_count;
    memcpy(records, s_scan_cache, count * sizeof(records[0]));
    xSemaphoreGive(s_wifi_mutex);

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) return web_send_json(request, NULL);
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddBoolToObject(json, "scanning", scanning);
    if (!scanning && result != 0) {
        cJSON_AddStringToObject(json, "error", wifi_scan_error_message(result));
    }

    cJSON *networks = cJSON_AddArrayToObject(json, "networks");
    for (size_t i = 0; networks != NULL && i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) break;
        cJSON_AddStringToObject(item, "ssid", records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddBoolToObject(item, "secure", records[i].authmode != 0);
        cJSON_AddItemToArray(networks, item);
    }
    return web_send_json(request, json);
}

static void wifi_config_worker(void *arg)
{
    wifi_config_context_t *context = arg;
    int result = wifi_prov_test_and_save(context->ssid, context->password);
    const char *message;
    wifi_job_state_t state = WIFI_JOB_FAILED;

    if (result == 0) {
        if (wifi_prov_schedule_restart(WIFI_RESTART_DELAY_MS) == 0) {
            state = WIFI_JOB_SUCCEEDED;
            message = "Wi-Fi verified and saved; gateway is restarting";
        } else {
            message = "Wi-Fi saved but restart could not be scheduled";
        }
    } else {
        message = wifi_config_error_message(result);
    }

    if (xSemaphoreTake(s_wifi_mutex, portMAX_DELAY) == pdTRUE) {
        s_job_state = state;
        strlcpy(s_job_message, message, sizeof(s_job_message));
        xSemaphoreGive(s_wifi_mutex);
    }
    free_wifi_context(context);
    vTaskDelete(NULL);
}

static esp_err_t wifi_status_get_handler(httpd_req_t *request)
{
    wifi_job_state_t state;
    char message[WIFI_JOB_MESSAGE_LEN];

    if (s_wifi_mutex == NULL ||
        xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Wi-Fi state is temporarily unavailable");
    }
    state = s_job_state;
    strlcpy(message, s_job_message, sizeof(message));
    xSemaphoreGive(s_wifi_mutex);

    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddStringToObject(json, "state", wifi_job_state_name(state));
        cJSON_AddStringToObject(json, "message", message);
    }
    return web_send_json(request, json);
}

static esp_err_t wifi_post_handler(httpd_req_t *request)
{
    char body[WEB_WIFI_BODY_MAX_LEN];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) {
        return web_send_body_error(request, body_status);
    }

    const char *ssid = web_get_json_string(json, "ssid", 33, true);
    const char *password = web_get_json_string(json, "password", 65, false);
    if (ssid == NULL ||
        (cJSON_GetObjectItemCaseSensitive(json, "password") != NULL &&
         password == NULL)) {
        cJSON_Delete(json);
        return web_send_api_error(request, "400 Bad Request",
                                  "Invalid SSID or password");
    }

    wifi_config_context_t *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        cJSON_Delete(json);
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Could not allocate Wi-Fi worker");
    }
    strlcpy(context->ssid, ssid, sizeof(context->ssid));
    strlcpy(context->password, password != NULL ? password : "",
            sizeof(context->password));
    cJSON_Delete(json);

    if (s_wifi_mutex == NULL ||
        xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free_wifi_context(context);
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Wi-Fi state is temporarily unavailable");
    }
    if (s_scan_running) {
        xSemaphoreGive(s_wifi_mutex);
        free_wifi_context(context);
        return web_send_api_error(request, "409 Conflict",
                                  "Wait for the Wi-Fi scan to finish");
    }
    if (s_job_state == WIFI_JOB_CONNECTING) {
        xSemaphoreGive(s_wifi_mutex);
        free_wifi_context(context);
        return web_send_api_error(request, "409 Conflict",
                                  "Wi-Fi configuration is already in progress");
    }

    s_job_state = WIFI_JOB_CONNECTING;
    strlcpy(s_job_message, "Testing Wi-Fi credentials", sizeof(s_job_message));
    xSemaphoreGive(s_wifi_mutex);

    if (xTaskCreate(wifi_config_worker, "wifi_config", WIFI_CONFIG_WORKER_STACK,
                    context, 5, NULL) != pdPASS) {
        if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_job_state = WIFI_JOB_FAILED;
            strlcpy(s_job_message, "Could not start Wi-Fi worker",
                    sizeof(s_job_message));
            xSemaphoreGive(s_wifi_mutex);
        }
        free_wifi_context(context);
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Could not start Wi-Fi worker");
    }

    httpd_resp_set_status(request, "202 Accepted");
    cJSON *response = cJSON_CreateObject();
    if (response != NULL) {
        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddStringToObject(response, "state", "connecting");
        cJSON_AddStringToObject(response, "message",
                               "Testing Wi-Fi credentials");
    }
    return web_send_json(request, response);
}

esp_err_t web_wifi_api_init(void)
{
    if (s_wifi_mutex == NULL) s_wifi_mutex = xSemaphoreCreateMutex();
    return s_wifi_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t web_wifi_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/wifi/scan", .method = HTTP_GET,
         .handler = wifi_scan_get_handler},
        {.uri = "/api/wifi/scan", .method = HTTP_POST,
         .handler = wifi_scan_post_handler},
        {.uri = "/api/wifi", .method = HTTP_GET,
         .handler = wifi_status_get_handler},
        {.uri = "/api/wifi", .method = HTTP_POST,
         .handler = wifi_post_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
