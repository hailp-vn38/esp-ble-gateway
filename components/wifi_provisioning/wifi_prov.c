#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wifi_prov.h"

static const char *TAG = "wifi_prov";
static const char *NVS_NAMESPACE = "wifi_cfg";

#define SOFTAP_SSID       "ESP32-Gateway-Setup"
#define SOFTAP_PASS       "gateway123"
#define SOFTAP_MAX_CONN    4
#define STA_CONNECT_RETRY  5

static bool s_sta_connected = false;
static int s_retry_count = 0;
static esp_netif_t *s_sta_netif = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        if (s_retry_count < STA_CONNECT_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_retry_count, STA_CONNECT_RETRY);
        } else {
            ESP_LOGE(TAG, "STA connect failed after %d retries.", STA_CONNECT_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        s_retry_count = 0;
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

static void start_softap(void)
{
    esp_netif_create_default_wifi_ap();

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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    ESP_LOGI(TAG, "SoftAP started: SSID=%s PASS=%s (open http://192.168.4.1)",
             SOFTAP_SSID, SOFTAP_PASS);
}

static void start_sta(const char *ssid, const char *pass)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s", ssid);
}

int wifi_prov_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t err = load_wifi_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    if (err == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Found saved Wi-Fi credentials, starting STA mode");
        start_sta(ssid, pass);
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi credentials, starting SoftAP for setup");
        start_softap();
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    return 0;
}

bool wifi_prov_is_connected(void) { return s_sta_connected; }

int wifi_prov_save_and_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0) return -1;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    nvs_set_str(handle, "ssid", ssid);
    nvs_set_str(handle, "pass", password ? password : "");
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "Wi-Fi credentials saved. Restart device to apply STA mode.");
    return 0;
}

void wifi_prov_get_ip(char *out_ip, size_t out_ip_len)
{
    if (s_sta_netif == NULL || !s_sta_connected) {
        strncpy(out_ip, "0.0.0.0", out_ip_len - 1);
        return;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
        snprintf(out_ip, out_ip_len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        strncpy(out_ip, "0.0.0.0", out_ip_len - 1);
    }
}
