#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include <stdlib.h>

#include "ble_central.h"
#include "board_io.h"
#include "command_dispatcher.h"
#include "command_executor.h"
#include "device_store.h"
#include "device_capabilities.h"
#include "gateway_ota_validate.h"
#include "mcp_endpoint.h"
#include "mcp_tool_exposure.h"
#include "mcp_ws_bridge.h"
#include "web_server.h"
#include "wifi_prov.h"

static const char *TAG = "app_main";

static void on_device_notify(const char *device_id, const gw_message_t *msg)
{
    if (device_capabilities_on_notify(device_id, msg)) return;
    command_dispatcher_on_device_notify(device_id, msg);
}

static void on_device_ready(const char *device_id)
{
    if (device_capabilities_on_ready(device_id) != ESP_OK) {
        ESP_LOGW(TAG, "[%s] capability discovery could not be queued", device_id);
    }
}

typedef struct {
    device_cap_submit_done_fn done;
    void *context;
} capability_submit_bridge_t;

static void capability_executor_completion(const dispatch_result_t *result,
                                           void *context)
{
    capability_submit_bridge_t *bridge = context;
    device_cap_submit_result_t outcome = DEVICE_CAP_SUBMIT_INTERNAL_ERROR;
    if (result != NULL) {
        switch (result->status) {
        case DISPATCH_STATUS_OK:
            outcome = DEVICE_CAP_SUBMIT_OK;
            break;
        case DISPATCH_STATUS_DEVICE_ERROR:
        case DISPATCH_STATUS_UNSUPPORTED_COMMAND:
            outcome = DEVICE_CAP_SUBMIT_REJECTED;
            break;
        case DISPATCH_STATUS_BUSY:
        case DISPATCH_STATUS_CONFLICT:
            outcome = DEVICE_CAP_SUBMIT_BUSY;
            break;
        case DISPATCH_STATUS_TIMEOUT:
            outcome = DEVICE_CAP_SUBMIT_TIMEOUT;
            break;
        case DISPATCH_STATUS_NOT_CONNECTED:
            outcome = DEVICE_CAP_SUBMIT_NOT_CONNECTED;
            break;
        case DISPATCH_STATUS_TRANSPORT_ERROR:
            outcome = DEVICE_CAP_SUBMIT_TRANSPORT_ERROR;
            break;
        default:
            outcome = DEVICE_CAP_SUBMIT_INTERNAL_ERROR;
            break;
        }
    }
    bridge->done(outcome, bridge->context);
    free(bridge);
}

static esp_err_t capability_submit(const gw_message_t *message,
                                   device_cap_submit_done_fn done,
                                   void *context)
{
    capability_submit_bridge_t *bridge = malloc(sizeof(*bridge));
    if (bridge == NULL) return ESP_ERR_NO_MEM;
    bridge->done = done;
    bridge->context = context;
    esp_err_t error = command_executor_submit(
        message, capability_executor_completion, bridge);
    if (error != ESP_OK) free(bridge);
    return error;
}

static void on_board_io_event(board_io_event_t event, void *context)
{
    (void)context;

    switch (event) {
    case BOARD_IO_EVENT_BUTTON_SHORT_PRESS:
        ESP_LOGI(TAG, "Button short press (no action assigned yet)");
        break;
    case BOARD_IO_EVENT_RESTART_REQUEST:
        ESP_LOGW(TAG, "Restart requested via button");
        esp_restart();
        break;
    case BOARD_IO_EVENT_FACTORY_RESET_REQUEST:
        ESP_LOGW(TAG, "Factory reset requested via button: clearing Wi-Fi credentials");
        if (wifi_prov_clear_credentials() != ESP_OK) {
            ESP_LOGE(TAG, "Clearing Wi-Fi credentials failed");
        }
        esp_restart();
        break;
    default:
        break;
    }
}

/* Event-driven board status: called synchronously by wifi_prov whenever the
 * workflow state changes, replacing the 300 ms polling task (Plan v1.1 §16). */
static void on_wifi_prov_state_change(wifi_prov_state_t new_state, void *ctx)
{
    (void)ctx;
    board_status_t resolved;
    switch (new_state) {
    case WIFI_PROV_STATE_BOOT_CONNECTING:
    case WIFI_PROV_STATE_RECONNECTING:
        resolved = BOARD_STATUS_WIFI_CONNECTING;
        break;
    case WIFI_PROV_STATE_PROVISIONING:
    case WIFI_PROV_STATE_TESTING:
    case WIFI_PROV_STATE_RESTART_PENDING:
        resolved = BOARD_STATUS_PROVISIONING;
        break;
    case WIFI_PROV_STATE_CONNECTED:
        resolved = BOARD_STATUS_READY;
        break;
    case WIFI_PROV_STATE_FAILED:
        resolved = BOARD_STATUS_ERROR;
        break;
    case WIFI_PROV_STATE_UNINITIALIZED:
    default:
        resolved = BOARD_STATUS_BOOTING;
        break;
    }
    board_io_set_status(resolved);
}

void app_main(void)
{
    esp_err_t io_rc = board_io_init();
    if (io_rc == ESP_OK) {
        if (board_io_register_event_handler(on_board_io_event, NULL) != ESP_OK) {
            ESP_LOGW(TAG, "Board I/O event handler registration failed");
        }
        board_io_set_status(BOARD_STATUS_BOOTING);
    } else {
        ESP_LOGE(TAG, "Board I/O init failed: %s", esp_err_to_name(io_rc));
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // OTA rollback validation — must run before any gateway services start.
    if (gateway_ota_validate() != ESP_OK) {
        ESP_LOGE(TAG, "OTA validation failed; gateway services not started");
        return;
    }

    if (wifi_prov_init() != 0) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed; gateway services were not started");
        return;
    }

    /* Register event-driven board status observer (Plan v1.1 §16). */
    wifi_prov_register_state_observer(on_wifi_prov_state_change, NULL);
    /* Push initial state after observer is registered. */
    on_wifi_prov_state_change(wifi_prov_get_state(), NULL);

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

    if (device_store_init() != DEVICE_STORE_OK) {
        ESP_LOGE(TAG, "Device store initialization failed");
        return;
    }
    if (device_capabilities_init() != ESP_OK) {
        ESP_LOGE(TAG, "Device capability manager initialization failed");
        return;
    }
    if (mcp_tool_exposure_init() != ESP_OK) {
        ESP_LOGE(TAG, "MCP tool exposure initialization failed");
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
    if (command_executor_init() != ESP_OK) {
        ESP_LOGE(TAG, "Command executor initialization failed");
        return;
    }
    device_capabilities_set_submitter(capability_submit);
    if (ble_central_init(on_device_notify) != 0) {
        ESP_LOGE(TAG, "BLE central initialization failed");
        return;
    }
    ble_central_set_lifecycle_callbacks(on_device_ready,
                                        device_capabilities_on_disconnect);
    if (ble_central_start_reconnect_supervisor() != 0) {
        ESP_LOGE(TAG, "BLE reconnect supervisor could not be started");
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

    if (mcp_ws_bridge_is_supported()) {
        mcp_ws_config_t bridge_config = {0};
        esp_err_t cfg_err = mcp_ws_bridge_config_load(&bridge_config);
        if (cfg_err == ESP_OK && bridge_config.enabled) {
            esp_err_t bridge_result = mcp_ws_bridge_init();
            if (bridge_result == ESP_OK) {
                bridge_result = mcp_ws_bridge_start();
            }
            if (bridge_result != ESP_OK) {
                ESP_LOGW(TAG, "External MCP bridge unavailable: %s",
                         esp_err_to_name(bridge_result));
            }
        } else {
            ESP_LOGI(TAG, "External MCP bridge disabled; skipping init");
        }
        memset(&bridge_config, 0, sizeof(bridge_config));
    }

    ESP_LOGI(TAG, "ESP32 BLE Gateway started (Central + Web UI + JSON-RPC)");
}
