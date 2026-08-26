#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
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

#define PROV_EVT_STA_GOT_IP BIT0
#define PROV_EVT_STA_FAILED BIT1
#define PROV_EVT_AP_STARTED BIT2

#define AP_READY_TIMEOUT_MS 5000
#define OPERATION_LOCK_TIMEOUT_MS 1000
#define RUNTIME_RECONNECT_LIMIT CONFIG_WIFI_PROV_STA_BOOT_RETRY_COUNT

/* Legacy result codes (compatibility contract with web_wifi_api.c):
 * 0 success | -1 invalid arg / generic init | -2 invalid state
 * -3 busy | -4 Wi-Fi operation could not start
 * -5 connect timeout / verification failed | -6 NVS persistence failed */

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

/* Workflow state (§5.4: workflow state and physical link state are distinct). */
static wifi_prov_state_t s_state = WIFI_PROV_STATE_UNINITIALIZED;

/* Physical fact: STA currently holds an IPv4 lease. */
static bool s_sta_has_ip;

/* Semantic flag: disconnect events are allowed to trigger a reconnect. */
static bool s_sta_retry_enabled;

static int s_retry_count;
static bool s_restart_scheduled;

/* Resources owned directly by this component. */
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_wifi_handler_registered;
static bool s_ip_handler_registered;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_operation_mutex;

static char s_ap_ssid[33];

/* ------------------------------------------------------------------ */
/* State helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_state(wifi_prov_state_t new_state)
{
    wifi_prov_state_t old_state;
    portENTER_CRITICAL(&s_state_lock);
    old_state = s_state;
    s_state = new_state;
    portEXIT_CRITICAL(&s_state_lock);

    if (old_state != new_state) {
        ESP_LOGI(TAG, "state: %s -> %s", wifi_prov_state_name(old_state),
                 wifi_prov_state_name(new_state));
    }
}

static wifi_prov_state_t get_state(void)
{
    portENTER_CRITICAL(&s_state_lock);
    wifi_prov_state_t state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

static void set_sta_has_ip(bool value)
{
    portENTER_CRITICAL(&s_state_lock);
    s_sta_has_ip = value;
    portEXIT_CRITICAL(&s_state_lock);
}

static void set_sta_retry_enabled(bool value)
{
    portENTER_CRITICAL(&s_state_lock);
    s_sta_retry_enabled = value;
    portEXIT_CRITICAL(&s_state_lock);
}

static bool get_sta_retry_enabled(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool value = s_sta_retry_enabled;
    portEXIT_CRITICAL(&s_state_lock);
    return value;
}

static int increment_retry(int limit, bool *exhausted)
{
    portENTER_CRITICAL(&s_state_lock);
    s_retry_count++;
    *exhausted = s_retry_count > limit;
    int count = s_retry_count;
    portEXIT_CRITICAL(&s_state_lock);
    return count;
}

static void reset_retry(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_retry_count = 0;
    portEXIT_CRITICAL(&s_state_lock);
}

const char *wifi_prov_state_name(wifi_prov_state_t state)
{
    switch (state) {
    case WIFI_PROV_STATE_UNINITIALIZED: return "uninitialized";
    case WIFI_PROV_STATE_BOOT_CONNECTING: return "boot_connecting";
    case WIFI_PROV_STATE_PROVISIONING: return "provisioning";
    case WIFI_PROV_STATE_TESTING: return "testing";
    case WIFI_PROV_STATE_RESTART_PENDING: return "restart_pending";
    case WIFI_PROV_STATE_CONNECTED: return "connected";
    case WIFI_PROV_STATE_RECONNECTING: return "reconnecting";
    case WIFI_PROV_STATE_FAILED: return "failed";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* NVS helpers                                                         */
/* ------------------------------------------------------------------ */

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

esp_err_t wifi_prov_clear_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_erase_key(handle, "ssid");
    if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK) {
        error = nvs_erase_key(handle, "pass");
        if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    }
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    /* Caller decides whether to reboot; no runtime HTTP mode hot-switch. */
    return error;
}

/* ------------------------------------------------------------------ */
/* Config helpers                                                      */
/* ------------------------------------------------------------------ */

static void generate_ap_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    const char *prefix = CONFIG_WIFI_PROV_AP_PREFIX;
    /* Keep room for "-A1B2C3" suffix within the 32-byte SSID limit. */
    size_t max_prefix = sizeof(s_ap_ssid) - 1 - 7;
    size_t prefix_len = strlen(prefix);
    if (prefix_len > max_prefix) prefix_len = max_prefix;

    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%.*s-%02X%02X%02X",
             (int)prefix_len, prefix, mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Generated SoftAP SSID: %s", s_ap_ssid);
}

static esp_err_t validate_config(void)
{
    const char *password = CONFIG_WIFI_PROV_AP_PASSWORD;
    size_t length = strlen(password);
    if (length > 0 && (length < 8 || length > 63)) {
        ESP_LOGE(TAG,
                 "CONFIG_WIFI_PROV_AP_PASSWORD must be empty (open AP) "
                 "or 8..63 characters for WPA2");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t configure_softap(void)
{
    wifi_config_t config = {
        .ap = {
            .max_connection = CONFIG_WIFI_PROV_AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    const char *password = CONFIG_WIFI_PROV_AP_PASSWORD;
    if (password[0] == '\0') config.ap.authmode = WIFI_AUTH_OPEN;

    strlcpy((char *)config.ap.ssid, s_ap_ssid, sizeof(config.ap.ssid));
    config.ap.ssid_len = (uint8_t)strnlen(s_ap_ssid, sizeof(config.ap.ssid));
    strlcpy((char *)config.ap.password, password, sizeof(config.ap.password));
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

static void apply_power_save_policy(void)
{
    esp_err_t error = esp_wifi_set_ps(WIFI_PS_NONE);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not disable Wi-Fi power save: %s",
                 esp_err_to_name(error));
    } else {
        ESP_LOGI(TAG,
                 "Wi-Fi power save disabled for low-latency gateway operation");
    }
}

static esp_err_t ensure_apsta_mode(void)
{
    wifi_mode_t mode;
    esp_err_t error = esp_wifi_get_mode(&mode);
    if (error != ESP_OK) return error;
    return mode == WIFI_MODE_APSTA ? ESP_OK : esp_wifi_set_mode(WIFI_MODE_APSTA);
}

/* ------------------------------------------------------------------ */
/* Connect attempt helpers                                             */
/* ------------------------------------------------------------------ */

static esp_err_t start_sta_attempt(const char *ssid, const char *password)
{
    xEventGroupClearBits(s_wifi_events, PROV_EVT_STA_GOT_IP | PROV_EVT_STA_FAILED);
    reset_retry();
    set_sta_retry_enabled(true);

    esp_err_t error = configure_sta(ssid, password);
    if (error == ESP_OK) error = esp_wifi_connect();
    if (error != ESP_OK) set_sta_retry_enabled(false);
    return error;
}

static void stop_sta_attempt(void)
{
    set_sta_retry_enabled(false);
    esp_wifi_disconnect();
}

static EventBits_t wait_sta_result(TickType_t timeout_ticks)
{
    return xEventGroupWaitBits(s_wifi_events,
                               PROV_EVT_STA_GOT_IP | PROV_EVT_STA_FAILED,
                               pdFALSE, pdFALSE, timeout_ticks);
}

/* ------------------------------------------------------------------ */
/* Wi-Fi event handler                                                 */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (get_sta_retry_enabled()) esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        xEventGroupSetBits(s_wifi_events, PROV_EVT_AP_STARTED);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        set_sta_has_ip(false);
        bool retry_enabled = get_sta_retry_enabled();
        wifi_prov_state_t state = get_state();

        switch (state) {
        case WIFI_PROV_STATE_BOOT_CONNECTING:
        case WIFI_PROV_STATE_TESTING:
            /* Credential worker owns fallback; never touch AP/DNS here. */
            if (!retry_enabled) return;
            {
                bool exhausted = false;
                int count =
                    increment_retry(CONFIG_WIFI_PROV_STA_BOOT_RETRY_COUNT,
                                    &exhausted);
                if (!exhausted) {
                    ESP_LOGW(TAG, "STA disconnected, retry %d/%d", count,
                             CONFIG_WIFI_PROV_STA_BOOT_RETRY_COUNT);
                    esp_wifi_connect();
                } else {
                    set_sta_retry_enabled(false);
                    xEventGroupSetBits(s_wifi_events, PROV_EVT_STA_FAILED);
                    ESP_LOGE(TAG, "Credential connect failed after %d retries",
                             CONFIG_WIFI_PROV_STA_BOOT_RETRY_COUNT);
                }
            }
            return;

        case WIFI_PROV_STATE_CONNECTED:
        case WIFI_PROV_STATE_RECONNECTING:
            /* Runtime disconnect never opens the provisioning portal. */
            if (!retry_enabled) return;
            {
                bool exhausted = false;
                increment_retry(RUNTIME_RECONNECT_LIMIT, &exhausted);
                set_state(WIFI_PROV_STATE_RECONNECTING);
                if (!exhausted) {
                    esp_wifi_connect();
                } else {
                    set_sta_retry_enabled(false);
                    set_state(WIFI_PROV_STATE_FAILED);
                    ESP_LOGE(TAG,
                             "Runtime reconnect exhausted; external action required");
                }
            }
            return;

        case WIFI_PROV_STATE_PROVISIONING:
        case WIFI_PROV_STATE_RESTART_PENDING:
        default:
            /* AP/DNS stay untouched. */
            return;
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        set_sta_has_ip(true);
        reset_retry();
        xEventGroupSetBits(s_wifi_events, PROV_EVT_STA_GOT_IP);
        if (get_state() == WIFI_PROV_STATE_RECONNECTING)
            set_state(WIFI_PROV_STATE_CONNECTED);
    }
}

/* ------------------------------------------------------------------ */
/* Provisioning entry                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t wait_for_ap_ready(TickType_t timeout)
{
    TickType_t deadline = xTaskGetTickCount() + timeout;
    for (;;) {
        if (s_ap_netif != NULL && esp_netif_is_netif_up(s_ap_netif))
            return ESP_OK;

        xEventGroupClearBits(s_wifi_events, PROV_EVT_AP_STARTED);
        /* Re-check after clear to close the missed-event window. */
        if (s_ap_netif != NULL && esp_netif_is_netif_up(s_ap_netif))
            return ESP_OK;

        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) break;
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events, PROV_EVT_AP_STARTED, pdFALSE, pdFALSE,
            deadline - now);
        if ((bits & PROV_EVT_AP_STARTED) != 0 &&
            s_ap_netif != NULL && esp_netif_is_netif_up(s_ap_netif)) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t enter_provisioning(void)
{
    esp_err_t error = ensure_apsta_mode();
    if (error == ESP_OK) error = configure_softap();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure SoftAP: %s",
                 esp_err_to_name(error));
        set_state(WIFI_PROV_STATE_FAILED);
        return error;
    }

    if (wait_for_ap_ready(pdMS_TO_TICKS(AP_READY_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "SoftAP did not confirm readiness in %d ms",
                 AP_READY_TIMEOUT_MS);
    }

    esp_netif_ip_info_t ap_ip_info;
    error = esp_netif_get_ip_info(s_ap_netif, &ap_ip_info);
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "SoftAP IPv4 for captive DNS: " IPSTR,
                 IP2STR(&ap_ip_info.ip));
        error = dns_hijack_start(&ap_ip_info.ip);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS did not start (%s); provisioning is "
                          "still available by IP",
                     esp_err_to_name(error));
        }
    } else {
        ESP_LOGW(TAG, "Could not read SoftAP IPv4 (%s); captive DNS skipped",
                 esp_err_to_name(error));
    }

    set_state(WIFI_PROV_STATE_PROVISIONING);
    ESP_LOGI(TAG, "Provisioning portal ready: SSID=%s", s_ap_ssid);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Scan                                                                */
/* ------------------------------------------------------------------ */

int wifi_prov_scan(wifi_prov_ap_record_t *records, size_t max_records,
                   size_t *out_count)
{
    if (records == NULL || out_count == NULL || max_records == 0) return -1;
    *out_count = 0;
    if (get_state() != WIFI_PROV_STATE_PROVISIONING) return -2;
    if (xSemaphoreTake(s_operation_mutex,
                       pdMS_TO_TICKS(OPERATION_LOCK_TIMEOUT_MS)) != pdTRUE)
        return -3;

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = 20,
            .max = 60,
        },
        .home_chan_dwell_time = 30,
        .coex_background_scan = true,
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
                if (strcmp(records[j].ssid,
                           (const char *)raw_records[i].ssid) == 0) {
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

/* ------------------------------------------------------------------ */
/* Credential test-and-save                                            */
/* ------------------------------------------------------------------ */

int wifi_prov_test_and_save(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strnlen(ssid, 33) >= 33 ||
        (password != NULL && strnlen(password, 65) >= 65)) {
        return -1;
    }
    if (!wifi_prov_is_provisioning()) return -2;
    if (xSemaphoreTake(s_operation_mutex,
                       pdMS_TO_TICKS(OPERATION_LOCK_TIMEOUT_MS)) != pdTRUE)
        return -3;

    const char *safe_password = password != NULL ? password : "";
    /* Clear any stale attempt before testing new credentials. */
    stop_sta_attempt();
    set_state(WIFI_PROV_STATE_TESTING);
    ESP_LOGI(TAG, "Testing Wi-Fi SSID: %s", ssid);

    esp_err_t error = ensure_apsta_mode();
    if (error == ESP_OK) error = start_sta_attempt(ssid, safe_password);
    if (error != ESP_OK) {
        set_state(WIFI_PROV_STATE_PROVISIONING);
        xSemaphoreGive(s_operation_mutex);
        return -4;
    }

    EventBits_t bits = wait_sta_result(
        pdMS_TO_TICKS(CONFIG_WIFI_PROV_STA_TEST_TIMEOUT_MS));

    if ((bits & PROV_EVT_STA_GOT_IP) == 0) {
        ESP_LOGE(TAG, "Credential test failed for SSID: %s", ssid);
        stop_sta_attempt();
        set_state(WIFI_PROV_STATE_PROVISIONING);
        xSemaphoreGive(s_operation_mutex);
        return -5;
    }

    /* Persist only after GOT_IP (Invariant B). */
    error = save_wifi_credentials(ssid, safe_password);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed (%s); keeping provisioning active",
                 esp_err_to_name(error));
        stop_sta_attempt();
        set_state(WIFI_PROV_STATE_PROVISIONING);
        xSemaphoreGive(s_operation_mutex);
        return -6;
    }

    /* No intentional disconnect and no DNS/AP teardown here: the
     * provisioning client polls /api/wifi over SoftAP before the
     * delayed restart. */
    set_sta_retry_enabled(false);
    set_state(WIFI_PROV_STATE_RESTART_PENDING);
    xSemaphoreGive(s_operation_mutex);
    ESP_LOGI(TAG, "Wi-Fi verified and saved: %s", ssid);
    return 0;
}

int wifi_prov_save_and_connect(const char *ssid, const char *password)
{
    return wifi_prov_test_and_save(ssid, password); // legacy alias
}

/* ------------------------------------------------------------------ */
/* Restart scheduling                                                  */
/* ------------------------------------------------------------------ */

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
        xSemaphoreTake(s_operation_mutex,
                       pdMS_TO_TICKS(OPERATION_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return -1;
    }
    if (get_state() != WIFI_PROV_STATE_RESTART_PENDING) {
        ESP_LOGW(TAG, "Restart requested outside RESTART_PENDING (state=%s)",
                 wifi_prov_state_name(get_state()));
    }
    if (s_restart_scheduled) {
        ESP_LOGI(TAG, "Restart already scheduled");
        xSemaphoreGive(s_operation_mutex);
        return 0;
    }
    s_restart_scheduled = true;
    xSemaphoreGive(s_operation_mutex);

    if (xTaskCreate(restart_task, "wifi_restart", 2048,
                    (void *)(uintptr_t)delay_ms, 5, NULL) != pdPASS) {
        if (xSemaphoreTake(s_operation_mutex,
                           pdMS_TO_TICKS(OPERATION_LOCK_TIMEOUT_MS)) == pdTRUE) {
            s_restart_scheduled = false;
            xSemaphoreGive(s_operation_mutex);
        }
        ESP_LOGE(TAG, "Could not create restart task");
        return -1;
    }
    ESP_LOGI(TAG, "Restart scheduled in %u ms", delay_ms);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Status queries                                                      */
/* ------------------------------------------------------------------ */

bool wifi_prov_is_connected(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool has_ip = s_sta_has_ip;
    portEXIT_CRITICAL(&s_state_lock);
    return has_ip;
}

bool wifi_prov_is_provisioning(void)
{
    wifi_prov_state_t state = get_state();
    return state == WIFI_PROV_STATE_PROVISIONING ||
           state == WIFI_PROV_STATE_TESTING ||
           state == WIFI_PROV_STATE_RESTART_PENDING;
}

wifi_prov_state_t wifi_prov_get_state(void)
{
    return get_state();
}

void wifi_prov_get_ip(char *out_ip, size_t out_ip_len)
{
    if (out_ip == NULL || out_ip_len == 0) return;
    portENTER_CRITICAL(&s_state_lock);
    bool has_ip = s_sta_has_ip;
    portEXIT_CRITICAL(&s_state_lock);

    esp_netif_t *netif =
        has_ip ? s_sta_netif
               : (wifi_prov_is_provisioning() ? s_ap_netif : NULL);
    esp_netif_ip_info_t ip_info;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        snprintf(out_ip, out_ip_len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        strlcpy(out_ip, "0.0.0.0", out_ip_len);
    }
}

/* ------------------------------------------------------------------ */
/* Init + cleanup                                                      */
/* ------------------------------------------------------------------ */

static void cleanup_owned_resources(void)
{
    if (dns_hijack_is_running()) dns_hijack_stop();
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_ip_handler_registered) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     wifi_event_handler);
        s_ip_handler_registered = false;
    }
    if (s_wifi_handler_registered) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler);
        s_wifi_handler_registered = false;
    }
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_wifi_events != NULL) {
        vEventGroupDelete(s_wifi_events);
        s_wifi_events = NULL;
    }
    if (s_operation_mutex != NULL) {
        vSemaphoreDelete(s_operation_mutex);
        s_operation_mutex = NULL;
    }
    set_sta_retry_enabled(false);
    set_sta_has_ip(false);
    s_restart_scheduled = false;
    set_state(WIFI_PROV_STATE_UNINITIALIZED);
}

int wifi_prov_init(void)
{
    if (s_wifi_initialized) {
        ESP_LOGE(TAG, "wifi_prov_init called twice in the same boot");
        return -1;
    }
    if (validate_config() != ESP_OK) return -1;

    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return -1;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return -1;

    s_wifi_events = xEventGroupCreate();
    s_operation_mutex = xSemaphoreCreateMutex();
    if (s_wifi_events == NULL || s_operation_mutex == NULL) goto fail;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) goto fail;

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_config) != ESP_OK) goto fail;
    s_wifi_initialized = true;

    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) goto fail;
    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event_handler, NULL) == ESP_OK)
        s_wifi_handler_registered = true;
    if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   wifi_event_handler, NULL) == ESP_OK)
        s_ip_handler_registered = true;
    if (!s_wifi_handler_registered || !s_ip_handler_registered) goto fail;

    generate_ap_ssid();

    char ssid[33] = {0};
    char saved_password[65] = {0};
    bool has_credentials =
        load_wifi_credentials(ssid, sizeof(ssid), saved_password,
                              sizeof(saved_password)) == ESP_OK &&
        ssid[0] != '\0';

    if (has_credentials) {
        ESP_LOGI(TAG, "Trying saved Wi-Fi credentials (SSID=%s)", ssid);
        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) goto fail;
        if (esp_wifi_start() != ESP_OK) goto fail;
        s_wifi_started = true;
        apply_power_save_policy();
        set_state(WIFI_PROV_STATE_BOOT_CONNECTING);

        if (start_sta_attempt(ssid, saved_password) != ESP_OK) {
            stop_sta_attempt();
            if (enter_provisioning() != ESP_OK) goto fail;
            return 0;
        }

        EventBits_t bits = wait_sta_result(
            pdMS_TO_TICKS(CONFIG_WIFI_PROV_STA_BOOT_TIMEOUT_MS));
        if ((bits & PROV_EVT_STA_GOT_IP) != 0) {
            /* Keep retry enabled: runtime disconnects must trigger bounded
             * reconnect, never the provisioning portal. */
            reset_retry();
            set_state(WIFI_PROV_STATE_CONNECTED);
            ESP_LOGI(TAG, "Saved Wi-Fi verified; running in STA mode");
        } else {
            ESP_LOGW(TAG, "Saved credential boot connect failed; entering "
                          "provisioning");
            stop_sta_attempt();
            if (enter_provisioning() != ESP_OK) goto fail;
        }
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi; entering provisioning mode");
        if (ensure_apsta_mode() != ESP_OK || configure_softap() != ESP_OK)
            goto fail;
        if (esp_wifi_start() != ESP_OK) goto fail;
        s_wifi_started = true;
        apply_power_save_policy();
        if (enter_provisioning() != ESP_OK) goto fail;
    }

    return 0;

fail:
    cleanup_owned_resources();
    return -1;
}
