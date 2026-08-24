#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "dns_hijack.h"
#include "wifi_prov.h"

static const char *TAG = "wifi_prov";
static const char *NVS_NAMESPACE = "wifi_cfg";

#define SOFTAP_SSID          "ESP32-Gateway-Setup"
#define SOFTAP_PASS          "gateway123"
#define SOFTAP_MAX_CONN       4
#define STA_CONNECT_RETRY     5
#define STA_BOOT_TIMEOUT_MS  30000
#define STA_TEST_TIMEOUT_MS  20000

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

static volatile wifi_prov_state_t s_state = WIFI_PROV_STATE_SOFTAP;
static volatile bool s_sta_connected;
static volatile bool s_provisioning_active;
static volatile bool s_connect_requested;
static volatile bool s_testing_credentials;
static volatile bool s_fallback_scheduled;
static volatile bool s_restart_scheduled;
static int s_retry_count;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_operation_mutex;

static esp_err_t start_softap(void);

static void fallback_task(void *arg)
{
    if (xSemaphoreTake(s_operation_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        start_softap();
        xSemaphoreGive(s_operation_mutex);
    }
    s_fallback_scheduled = false;
    vTaskDelete(NULL);
}

static void schedule_softap_fallback(void)
{
    if (s_fallback_scheduled) return;
    s_fallback_scheduled = true;
    if (xTaskCreate(fallback_task, "wifi_fallback", 3072, NULL, 5, NULL) != pdPASS) {
        s_fallback_scheduled = false;
        ESP_LOGE(TAG, "Could not schedule SoftAP fallback");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_connect_requested) esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (!s_connect_requested) return;

        s_state = WIFI_PROV_STATE_CONNECTING;
        if (++s_retry_count <= STA_CONNECT_RETRY) {
            esp_wifi_connect();
            ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_retry_count,
                     STA_CONNECT_RETRY);
        } else {
            s_connect_requested = false;
            s_state = WIFI_PROV_STATE_FAILED;
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
            ESP_LOGE(TAG, "STA connection failed after %d retries", STA_CONNECT_RETRY);
            if (!s_testing_credentials) schedule_softap_fallback();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        s_state = WIFI_PROV_STATE_CONNECTED;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t load_wifi_credentials(char *ssid, size_t ssid_len,
                                       char *password, size_t password_len)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    error = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    if (error == ESP_OK) error = nvs_get_str(handle, "pass", password, &password_len);
    nvs_close(handle);
    return error;
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_str(handle, "ssid", ssid);
    if (error == ESP_OK) error = nvs_set_str(handle, "pass", password);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

static esp_err_t configure_softap(void)
{
    wifi_config_t config = {
        .ap = {
            .ssid_len = sizeof(SOFTAP_SSID) - 1,
            .max_connection = SOFTAP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)config.ap.ssid, SOFTAP_SSID, sizeof(config.ap.ssid));
    strlcpy((char *)config.ap.password, SOFTAP_PASS, sizeof(config.ap.password));
    if (SOFTAP_PASS[0] == '\0') config.ap.authmode = WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_AP, &config);
}

static esp_err_t configure_sta(const char *ssid, const char *password)
{
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

static void stop_sta_connection(void)
{
    bool should_disconnect = s_connect_requested || s_sta_connected;
    s_connect_requested = false;
    if (should_disconnect) esp_wifi_disconnect();
}

static esp_err_t ensure_apsta_mode(void)
{
    wifi_mode_t mode;
    esp_err_t error = esp_wifi_get_mode(&mode);
    if (error != ESP_OK) return error;
    return mode == WIFI_MODE_APSTA ? ESP_OK : esp_wifi_set_mode(WIFI_MODE_APSTA);
}

static esp_err_t start_softap(void)
{
    stop_sta_connection();
    s_sta_connected = false;
    s_provisioning_active = true;
    s_state = WIFI_PROV_STATE_SOFTAP;
    s_retry_count = 0;

    esp_err_t error = ensure_apsta_mode();
    if (error == ESP_OK) error = configure_softap();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start SoftAP: %s", esp_err_to_name(error));
        s_state = WIFI_PROV_STATE_FAILED;
        return error;
    }
    if (dns_hijack_start() != 0) {
        ESP_LOGW(TAG, "Captive DNS did not start; provisioning is still available by IP");
    }
    ESP_LOGI(TAG, "SoftAP ready: SSID=%s, URL=http://192.168.4.1", SOFTAP_SSID);
    return ESP_OK;
}

int wifi_prov_init(void)
{
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return -1;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return -1;

    s_wifi_events = xEventGroupCreate();
    s_operation_mutex = xSemaphoreCreateMutex();
    if (s_wifi_events == NULL || s_operation_mutex == NULL) return -1;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) return -1;

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&init_config);
    if (error != ESP_OK) return -1;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return -1;
    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event_handler, NULL) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   wifi_event_handler, NULL) != ESP_OK) {
        return -1;
    }

    char ssid[33] = {0};
    char password[65] = {0};
    bool has_credentials = load_wifi_credentials(ssid, sizeof(ssid), password,
                                                 sizeof(password)) == ESP_OK &&
                           ssid[0] != '\0';

    if (has_credentials) {
        ESP_LOGI(TAG, "Trying saved Wi-Fi credentials");
        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
            configure_sta(ssid, password) != ESP_OK) {
            return -1;
        }
        s_connect_requested = true;
        s_testing_credentials = true;
        s_state = WIFI_PROV_STATE_CONNECTING;
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi; entering provisioning mode");
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || configure_softap() != ESP_OK) {
            return -1;
        }
        s_provisioning_active = true;
        s_state = WIFI_PROV_STATE_SOFTAP;
    }

    if (esp_wifi_start() != ESP_OK) return -1;
    error = esp_wifi_set_ps(WIFI_PS_NONE);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not disable Wi-Fi power save: %s", esp_err_to_name(error));
    } else {
        ESP_LOGI(TAG, "Wi-Fi power save disabled for low-latency gateway operation");
    }

    if (has_credentials) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(STA_BOOT_TIMEOUT_MS));
        s_testing_credentials = false;
        if ((bits & WIFI_CONNECTED_BIT) != 0) {
            s_provisioning_active = false;
            s_state = WIFI_PROV_STATE_CONNECTED;
            ESP_LOGI(TAG, "Saved Wi-Fi verified; running in STA mode");
        } else if (start_softap() != ESP_OK) {
            return -1;
        }
    } else {
        dns_hijack_start();
        ESP_LOGI(TAG, "SoftAP ready: SSID=%s, URL=http://192.168.4.1", SOFTAP_SSID);
    }
    return 0;
}

bool wifi_prov_is_connected(void)
{
    return s_sta_connected;
}

bool wifi_prov_is_provisioning(void)
{
    return s_provisioning_active;
}

wifi_prov_state_t wifi_prov_get_state(void)
{
    return s_state;
}

const char *wifi_prov_state_name(wifi_prov_state_t state)
{
    switch (state) {
    case WIFI_PROV_STATE_SOFTAP: return "softap";
    case WIFI_PROV_STATE_CONNECTING: return "connecting";
    case WIFI_PROV_STATE_CONNECTED: return "connected";
    case WIFI_PROV_STATE_FAILED: return "failed";
    default: return "unknown";
    }
}

int wifi_prov_scan(wifi_prov_ap_record_t *records, size_t max_records,
                   size_t *out_count)
{
    if (records == NULL || out_count == NULL || max_records == 0) return -1;
    *out_count = 0;
    if (!s_provisioning_active) return -2;
    if (xSemaphoreTake(s_operation_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return -3;

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t error = esp_wifi_scan_start(&scan_config, true);
    if (error != ESP_OK) {
        xSemaphoreGive(s_operation_mutex);
        return -4;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 32) ap_count = 32;
    wifi_ap_record_t raw_records[32] = {0};
    error = esp_wifi_scan_get_ap_records(&ap_count, raw_records);
    if (error == ESP_OK) {
        for (uint16_t i = 0; i < ap_count && *out_count < max_records; i++) {
            if (raw_records[i].ssid[0] == '\0') continue;
            bool duplicate = false;
            for (size_t j = 0; j < *out_count; j++) {
                if (strcmp(records[j].ssid, (const char *)raw_records[i].ssid) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            wifi_prov_ap_record_t *record = &records[(*out_count)++];
            memset(record, 0, sizeof(*record));
            strlcpy(record->ssid, (const char *)raw_records[i].ssid,
                    sizeof(record->ssid));
            record->rssi = raw_records[i].rssi;
            record->authmode = (uint8_t)raw_records[i].authmode;
        }
    }
    xSemaphoreGive(s_operation_mutex);
    return error == ESP_OK ? 0 : -4;
}

int wifi_prov_test_and_save(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strnlen(ssid, 33) >= 33 ||
        (password != NULL && strnlen(password, 65) >= 65)) {
        return -1;
    }
    if (!s_provisioning_active) return -2;
    if (xSemaphoreTake(s_operation_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return -3;

    const char *safe_password = password != NULL ? password : "";
    stop_sta_connection();
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_retry_count = 0;
    s_testing_credentials = true;
    s_state = WIFI_PROV_STATE_CONNECTING;

    esp_err_t error = ensure_apsta_mode();
    if (error == ESP_OK) error = configure_sta(ssid, safe_password);
    if (error == ESP_OK) {
        s_connect_requested = true;
        error = esp_wifi_connect();
    }
    if (error != ESP_OK) {
        s_testing_credentials = false;
        s_connect_requested = false;
        start_softap();
        xSemaphoreGive(s_operation_mutex);
        return -4;
    }

    ESP_LOGI(TAG, "Testing Wi-Fi SSID: %s", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(STA_TEST_TIMEOUT_MS));
    s_testing_credentials = false;

    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        start_softap();
        xSemaphoreGive(s_operation_mutex);
        return -5;
    }

    error = save_wifi_credentials(ssid, safe_password);
    if (error != ESP_OK) {
        start_softap();
        xSemaphoreGive(s_operation_mutex);
        return -6;
    }

    s_state = WIFI_PROV_STATE_CONNECTED;
    xSemaphoreGive(s_operation_mutex);
    ESP_LOGI(TAG, "Wi-Fi verified and saved: %s", ssid);
    return 0;
}

int wifi_prov_save_and_connect(const char *ssid, const char *password)
{
    return wifi_prov_test_and_save(ssid, password);
}

static void restart_task(void *arg)
{
    unsigned delay_ms = (unsigned)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    ESP_LOGI(TAG, "Restarting to apply verified Wi-Fi credentials");
    esp_restart();
}

int wifi_prov_schedule_restart(unsigned delay_ms)
{
    if (s_operation_mutex == NULL ||
        xSemaphoreTake(s_operation_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }
    if (s_restart_scheduled) {
        xSemaphoreGive(s_operation_mutex);
        return 0;
    }
    s_restart_scheduled = true;
    xSemaphoreGive(s_operation_mutex);

    if (xTaskCreate(restart_task, "wifi_restart", 2048,
                    (void *)(uintptr_t)delay_ms, 5, NULL) != pdPASS) {
        if (xSemaphoreTake(s_operation_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_restart_scheduled = false;
            xSemaphoreGive(s_operation_mutex);
        }
        return -1;
    }
    return 0;
}

void wifi_prov_get_ip(char *out_ip, size_t out_ip_len)
{
    if (out_ip == NULL || out_ip_len == 0) return;
    esp_netif_t *netif = s_sta_connected ? s_sta_netif
                                         : (s_provisioning_active ? s_ap_netif : NULL);
    esp_netif_ip_info_t ip_info;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(out_ip, out_ip_len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        strlcpy(out_ip, "0.0.0.0", out_ip_len);
    }
}
