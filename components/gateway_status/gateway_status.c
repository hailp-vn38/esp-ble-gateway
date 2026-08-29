#include "gateway_status.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "ble_central.h"
#include "device_store.h"
#include "wifi_prov.h"

esp_err_t gateway_status_get(gateway_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));

    device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
    size_t count = 0;
    if (device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES, &count) !=
        DEVICE_STORE_OK) {
        count = 0;
    }
    status->device_count = count;
    for (size_t i = 0; i < count; i++) {
        ble_central_device_status_t device_status;
        if (ble_central_get_device_status(devices[i].device_id,
                                           &device_status) == BLE_CENTRAL_OK &&
            device_status.connected) {
            status->connected_count++;
        }
    }
    status->ble_link_count = ble_central_active_count();

    wifi_prov_get_ip(status->ip, sizeof(status->ip));
    status->wifi_connected = wifi_prov_is_connected();
    status->provisioning = wifi_prov_is_provisioning();
    strlcpy(status->wifi_state,
            wifi_prov_state_name(wifi_prov_get_state()),
            sizeof(status->wifi_state));

    status->free_heap = esp_get_free_heap_size();
    status->uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);

    const esp_app_desc_t *app = esp_app_get_description();
    strlcpy(status->firmware_version, app->version,
            sizeof(status->firmware_version));
    strlcpy(status->idf_version, app->idf_ver, sizeof(status->idf_version));

    wifi_ap_record_t access_point = {0};
    if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
        strlcpy(status->wifi_ssid, (const char *)access_point.ssid,
                sizeof(status->wifi_ssid));
        status->has_wifi_rssi = true;
        status->wifi_rssi = access_point.rssi;
    }

    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(status->wifi_mac, sizeof(status->wifi_mac),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strlcpy(status->wifi_mac, "00:00:00:00:00:00", sizeof(status->wifi_mac));
    }

    // Internal SRAM telemetry
    status->internal_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    status->internal_min_free =
        (uint32_t)heap_caps_get_minimum_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    status->internal_largest_free_block =
        (uint32_t)heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // PSRAM telemetry
#if CONFIG_SPIRAM
    status->psram_ready = esp_psram_is_initialized();
#else
    status->psram_ready = false;
#endif
    if (status->psram_ready) {
        status->psram_free = (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        status->psram_min_free =
            (uint32_t)heap_caps_get_minimum_free_size(
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        status->psram_largest_free_block =
            (uint32_t)heap_caps_get_largest_free_block(
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    return ESP_OK;
}
