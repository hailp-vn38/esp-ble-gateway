#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_central.h"
#include "command_dispatcher.h"
#include "command_executor.h"
#include "device_store.h"
#include "log_buffer.h"
#include "mcp_endpoint.h"
#include "web_server.h"
#include "wifi_prov.h"

static const char *TAG = "app_main";

static void on_device_notify(const char *device_id, const gw_message_t *msg)
{
    command_dispatcher_on_device_notify(device_id, msg);
}

void app_main(void)
{
    log_buffer_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (wifi_prov_init() != 0) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed; gateway services were not started");
        return;
    }

    if (wifi_prov_is_provisioning()) {
        if (web_server_start_provisioning() == NULL) {
            ESP_LOGE(TAG, "Provisioning web server failed to start");
            return;
        }
        ESP_LOGI(TAG, "Provisioning mode started; gateway modules are deferred until restart");
        return;
    }

    if (!wifi_prov_is_connected()) {
        ESP_LOGE(TAG, "Wi-Fi is neither connected nor provisioning; gateway services deferred");
        return;
    }

    if (device_store_init() != 0) {
        ESP_LOGE(TAG, "Device store initialization failed");
        return;
    }
    if (command_dispatcher_init() != 0) {
        ESP_LOGE(TAG, "Command dispatcher initialization failed");
        return;
    }
    if (command_dispatcher_freeze_registry() != 0) {
        ESP_LOGE(TAG, "Command dispatcher registry freeze failed");
        return;
    }
    if (ble_central_init(on_device_notify) != 0) {
        ESP_LOGE(TAG, "BLE central initialization failed");
        return;
    }
    if (ble_central_start_reconnect_supervisor() != 0) {
        ESP_LOGE(TAG, "BLE reconnect supervisor could not be started");
        return;
    }
    if (command_executor_init() != ESP_OK) {
        ESP_LOGE(TAG, "Command executor initialization failed");
        return;
    }

    httpd_handle_t server = web_server_start();
    if (server != NULL) {
        if (mcp_endpoint_register(server) != 0) {
            ESP_LOGE(TAG, "MCP endpoint registration failed");
        }
    } else {
        ESP_LOGE(TAG, "Web server failed to start, /mcp endpoint not registered");
    }

    ESP_LOGI(TAG, "ESP32 BLE Gateway started (Central + Web UI + JSON-RPC)");
}
