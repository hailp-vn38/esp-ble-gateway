#include "web_server.h"

#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "web_modules.h"

static const char *TAG = "web_server";

// Route budget: assets 6 (dashboard + login + css + icons + font + favicon)
// + auth API 4 + mcp-token API 3 + settings API 3 + device API 4 + command API 1 + capability API 2 + exposure API 2
// + system API 3 + BLE API 3 = 31; headroom for future.
#define WEB_GATEWAY_MAX_URI_HANDLERS 34
#define WEB_GATEWAY_STACK_SIZE       12288

// Provisioning: assets 6 + system API 2 + Wi-Fi API 4 = 12.
#define WEB_PROVISIONING_MAX_URI_HANDLERS 14
#define WEB_PROVISIONING_STACK_SIZE       8192

typedef esp_err_t (*route_registrar_t)(httpd_handle_t server);

static httpd_handle_t start_server(const route_registrar_t *registrars,
                                   size_t registrar_count,
                                   unsigned max_handlers,
                                   unsigned stack_size,
                                   const char *mode_name)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.max_uri_handlers = max_handlers;
    config.stack_size = stack_size;
    config.task_priority = tskIDLE_PRIORITY + 6;
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    httpd_handle_t server = NULL;
    esp_err_t error = httpd_start(&server, &config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start %s web server: %s", mode_name,
                 esp_err_to_name(error));
        return NULL;
    }

    for (size_t i = 0; i < registrar_count; i++) {
        if (registrars[i](server) != ESP_OK) {
            httpd_stop(server);
            return NULL;
        }
    }

    ESP_LOGI(TAG, "%s web server started", mode_name);
    return server;
}

httpd_handle_t web_server_start(void)
{
    if (web_gateway_api_init() != ESP_OK || web_ble_api_init() != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize gateway web API state");
        return NULL;
    }
    if (web_event_ws_init() != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize WebSocket event state");
        return NULL;
    }

    static const route_registrar_t registrars[] = {
        web_assets_register_gateway,
        web_mcp_token_api_register,
        web_settings_api_register,
        web_gateway_api_register,
        web_system_api_register_gateway,
        web_ble_api_register,
    };
    httpd_handle_t server = start_server(
        registrars, sizeof(registrars) / sizeof(registrars[0]),
        WEB_GATEWAY_MAX_URI_HANDLERS, WEB_GATEWAY_STACK_SIZE, "Gateway");
    if (server != NULL) {
        /* WebSocket event endpoint: registered after server is running */
        if (web_event_ws_register(server) != ESP_OK) {
            ESP_LOGW(TAG, "/ws/events not registered; realtime disabled");
        }
    }
    return server;
}

httpd_handle_t web_server_start_provisioning(void)
{
    if (web_wifi_api_init() != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize provisioning web API state");
        return NULL;
    }

    static const route_registrar_t registrars[] = {
        web_assets_register_provisioning,
        web_system_api_register_provisioning,
        web_wifi_api_register,
    };
    httpd_handle_t server =
        start_server(registrars, sizeof(registrars) / sizeof(registrars[0]),
                     WEB_PROVISIONING_MAX_URI_HANDLERS,
                     WEB_PROVISIONING_STACK_SIZE, "Provisioning");
    if (server == NULL) return NULL;

    // Captive funnel for unknown URIs is provisioning-only; gateway mode
    // must keep plain 404s. Non-fatal: probes + DNS + option 114 survive.
    if (web_assets_register_provisioning_errors(server) != ESP_OK) {
        ESP_LOGW(TAG, "Captive 404 handler not registered; only known probe "
                      "routes redirect");
    }
    return server;
}
