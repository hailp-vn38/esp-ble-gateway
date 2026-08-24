#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_prov.h"

static const char *TAG = "wifi_prov";
static const char *NVS_NAMESPACE = "wifi_cfg";

#define SOFTAP_SSID       "ESP32-Gateway-Setup"
#define SOFTAP_PASS       "gateway123"
#define SOFTAP_MAX_CONN    4
#define STA_CONNECT_RETRY  5
#define STA_BOOT_TIMEOUT_MS 30000
#define STA_TEST_TIMEOUT_MS 20000

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

static bool s_sta_connected = false;
static bool s_provisioning_active = false;
static bool s_connect_requested = false;
static int s_retry_count = 0;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static EventGroupHandle_t s_wifi_events = NULL;
static SemaphoreHandle_t s_operation_mutex = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_connect_requested) esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (!s_connect_requested) return;

        if (++s_retry_count <= STA_CONNECT_RETRY) {
            esp_wifi_connect();
            ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_retry_count, STA_CONNECT_RETRY);
        } else {
            s_connect_requested = false;
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
            ESP_LOGE(TAG, "STA connect failed after %d retries", STA_CONNECT_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t load_wifi_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    err = nvs_get_str(handle, "pass", pass, &pass_len);
    nvs_close(handle);
    return err;
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "pass", password ? password : "");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t configure_softap(void)
{
    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(SOFTAP_SSID),
            .max_connection = SOFTAP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    strncpy((char *)ap_config.ap.ssid, SOFTAP_SSID, sizeof(ap_config.ap.ssid));
    strncpy((char *)ap_config.ap.password, SOFTAP_PASS, sizeof(ap_config.ap.password));
    if (strlen(SOFTAP_PASS) == 0) ap_config.ap.authmode = WIFI_AUTH_OPEN;

    return esp_wifi_set_config(WIFI_IF_AP, &ap_config);
}

static void stop_sta_connection(void)
{
    bool should_disconnect = s_connect_requested || s_sta_connected;
    s_connect_requested = false;
    if (should_disconnect) {
        // Ignore the return value: the connection may have ended between the
        // event callback and this state transition.
        esp_wifi_disconnect();
    }
}

static esp_err_t ensure_apsta_mode(void)
{
    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) return err;
    return (mode == WIFI_MODE_APSTA) ? ESP_OK : esp_wifi_set_mode(WIFI_MODE_APSTA);
}

static esp_err_t start_softap(void)
{
    stop_sta_connection();
    s_sta_connected = false;
    s_provisioning_active = true;

    esp_err_t err = ensure_apsta_mode();
    if (err == ESP_OK) err = configure_softap();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning SoftAP: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SoftAP started: SSID=%s PASS=%s (open http://192.168.4.1)",
             SOFTAP_SSID, SOFTAP_PASS);
    return ESP_OK;
}

static esp_err_t configure_sta(const char *ssid, const char *pass)
{
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    return esp_wifi_set_config(WIFI_IF_STA, &sta_config);
}

int wifi_prov_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_events = xEventGroupCreate();
    s_operation_mutex = xSemaphoreCreateMutex();
    if (s_wifi_events == NULL || s_operation_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to allocate Wi-Fi synchronization objects");
        return -1;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi network interfaces");
        return -1;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // Candidate credentials stay in RAM. Only our wifi_cfg namespace is persisted,
    // and only after a connection test has obtained an IP address.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t err = load_wifi_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    if (err == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Found saved Wi-Fi credentials, testing STA connection");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(configure_sta(ssid, pass));
        s_connect_requested = true;
        s_retry_count = 0;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi credentials, starting SoftAP for setup");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(configure_softap());
        s_provisioning_active = true;
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    if (s_connect_requested) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(STA_BOOT_TIMEOUT_MS));

        if ((bits & WIFI_CONNECTED_BIT) != 0) {
            s_provisioning_active = false;
            ESP_LOGI(TAG, "Saved Wi-Fi verified; gateway will run in STA mode");
        } else {
            ESP_LOGW(TAG, "Saved Wi-Fi is unavailable; falling back to provisioning mode");
            if (start_softap() != ESP_OK) return -1;
        }
    } else {
        ESP_LOGI(TAG, "Provisioning mode ready");
    }
    return 0;
}

bool wifi_prov_is_connected(void) { return s_sta_connected; }
bool wifi_prov_is_provisioning(void) { return s_provisioning_active; }

int wifi_prov_scan(wifi_prov_ap_record_t *records, size_t max_records, size_t *out_count)
{
    if (records == NULL || out_count == NULL || max_records == 0) return -1;
    *out_count = 0;
    if (!s_provisioning_active) return -2;
    if (xSemaphoreTake(s_operation_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return -3;

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        xSemaphoreGive(s_operation_mutex);
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        return -4;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 32) ap_count = 32;
    wifi_ap_record_t raw_records[32];
    memset(raw_records, 0, sizeof(raw_records));
    err = esp_wifi_scan_get_ap_records(&ap_count, raw_records);
    if (err != ESP_OK) {
        xSemaphoreGive(s_operation_mutex);
        return -4;
    }

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

        wifi_prov_ap_record_t *record = &records[*out_count];
        memset(record, 0, sizeof(*record));
        strncpy(record->ssid, (const char *)raw_records[i].ssid, sizeof(record->ssid) - 1);
        record->rssi = raw_records[i].rssi;
        record->authmode = (uint8_t)raw_records[i].authmode;
        (*out_count)++;
    }

    xSemaphoreGive(s_operation_mutex);
    ESP_LOGI(TAG, "Wi-Fi scan returned %d unique networks", (int)*out_count);
    return 0;
}

int wifi_prov_test_and_save(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) return -1;
    if (password != NULL && strlen(password) > 64) return -1;
    if (!s_provisioning_active) return -2;
    if (xSemaphoreTake(s_operation_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return -3;

    const char *safe_password = password ? password : "";
    stop_sta_connection();
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_retry_count = 0;

    esp_err_t err = ensure_apsta_mode();
    if (err == ESP_OK) err = configure_sta(ssid, safe_password);
    if (err != ESP_OK) {
        xSemaphoreGive(s_operation_mutex);
        ESP_LOGE(TAG, "Failed to configure candidate Wi-Fi: %s", esp_err_to_name(err));
        return -4;
    }

    s_connect_requested = true;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_connect_requested = false;
        xSemaphoreGive(s_operation_mutex);
        ESP_LOGE(TAG, "Failed to start candidate Wi-Fi connection: %s", esp_err_to_name(err));
        return -4;
    }

    ESP_LOGI(TAG, "Testing candidate Wi-Fi SSID: %s", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(STA_TEST_TIMEOUT_MS));

    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        stop_sta_connection();
        xSemaphoreGive(s_operation_mutex);
        ESP_LOGW(TAG, "Candidate Wi-Fi test failed: %s", ssid);
        return -5;
    }

    err = save_wifi_credentials(ssid, safe_password);
    if (err != ESP_OK) {
        xSemaphoreGive(s_operation_mutex);
        ESP_LOGE(TAG, "Candidate connected but could not be saved: %s", esp_err_to_name(err));
        return -6;
    }

    xSemaphoreGive(s_operation_mutex);
    ESP_LOGI(TAG, "Candidate Wi-Fi verified and saved: %s", ssid);
    return 0;
}

int wifi_prov_save_and_connect(const char *ssid, const char *password)
{
    return wifi_prov_test_and_save(ssid, password);
}

static void delayed_restart_task(void *arg)
{
    unsigned delay_ms = (unsigned)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

int wifi_prov_schedule_restart(unsigned delay_ms)
{
    BaseType_t rc = xTaskCreate(delayed_restart_task, "wifi_restart", 2048,
                                (void *)(uintptr_t)delay_ms, 5, NULL);
    return (rc == pdPASS) ? 0 : -1;
}

void wifi_prov_get_ip(char *out_ip, size_t out_ip_len)
{
    if (out_ip == NULL || out_ip_len == 0) return;
    out_ip[0] = '\0';

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = s_sta_connected ? s_sta_netif : (s_provisioning_active ? s_ap_netif : NULL);
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(out_ip, out_ip_len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(out_ip, out_ip_len, "0.0.0.0");
    }
}
