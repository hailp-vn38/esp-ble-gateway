#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

#include "device_store.h"
#include "wifi_prov.h"
#include "ble_central.h"
#include "cbor_codec.h"
#include "command_dispatcher.h"
#include "web_server.h"
#include "mcp_endpoint.h"
#include "log_buffer.h"

static const char *TAG = "app_main";

// Callback duoc goi tu ble_central moi khi thiet bi con Notify du lieu len
static void on_device_notify(const char *device_id, const gw_message_t *msg)
{
    command_dispatcher_on_device_notify(device_id, msg);
}

void app_main(void)
{
    // ---- 0. NVS (nen tang, bat buoc truoc tat ca) ----
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ---- 1. Module 1: device_store ----
    device_store_init();

    // ---- log_buffer (RAM, doc lap) ----
    log_buffer_init();

    // ---- 2. Module 2: wifi_provisioning (SoftAP hoac STA tuy NVS) ----
    if (wifi_prov_init() != 0) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed; gateway services were not started");
        return;
    }

    // ---- 3. Module 3: ble_central (NimBLE Central role) ----
    ble_central_init(on_device_notify);

    // ---- 5. Module 5: command_dispatcher (registry + router) ----
    command_dispatcher_init();

    // ---- 6. Module 6: web_server (HTTP server + Web UI API) ----
    httpd_handle_t server = web_server_start();

    // ---- 7. Module 7: mcp_endpoint (dang ky /mcp vao CUNG server voi Web UI) ----
    if (server != NULL) {
        mcp_endpoint_register(server);
    } else {
        ESP_LOGE(TAG, "Web server failed to start, /mcp endpoint not registered");
    }

    ESP_LOGI(TAG, "ESP32 BLE Gateway started - Giai doan 1 (Central + Web UI + MCP)");

    int device_count = 0;
    const device_entry_t *devices = device_store_list(&device_count);
    for (int i = 0; i < device_count; i++) {
        if (devices[i].has_ble_addr) {
            ble_central_connect(devices[i].device_id, devices[i].ble_addr,
                                devices[i].ble_addr_type);
        }
    }
}
