#include "web_modules.h"

#include "cJSON.h"
#include "esp_log.h"
#include "gateway_status.h"

#include "web_auth.h"
#include "web_auth_http.h"
#include "web_http.h"

static const char *TAG = "web_settings_api";

static esp_err_t settings_get_handler(httpd_req_t *request)
{
    web_auth_result_t auth = web_auth_require_request(request);
    if (auth != WEB_AUTH_OK) {
        return web_send_api_error_code(request, "401 Unauthorized",
                                       "Authentication required",
                                       "auth_required");
    }

    web_auth_status_t auth_status;
    if (web_auth_get_status(&auth_status) != ESP_OK) {
        return web_send_api_error_code(request, "500 Internal Server Error",
                                       "Could not read authentication status",
                                       "internal_error");
    }

    gateway_status_t gw_status;
    if (gateway_status_get(&gw_status) != ESP_OK) {
        return web_send_api_error_code(request, "500 Internal Server Error",
                                       "Could not read gateway status",
                                       "internal_error");
    }

    bool mcp_token_configured = false;
    char mcp_token_preview[8] = {0};
    esp_err_t token_error = web_mcp_token_get_status(
        &mcp_token_configured, mcp_token_preview,
        sizeof(mcp_token_preview));
    if (token_error != ESP_OK) {
        ESP_LOGW(TAG, "Could not read MCP token status: %s",
                 esp_err_to_name(token_error));
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);

    cJSON *system = cJSON_AddObjectToObject(response, "system");
    cJSON_AddStringToObject(system, "firmware", gw_status.firmware_version);
    cJSON_AddStringToObject(system, "idf", gw_status.idf_version);
    cJSON_AddNumberToObject(system, "uptime_ms", (double)gw_status.uptime_ms);
    cJSON_AddNumberToObject(system, "free_heap", gw_status.free_heap);

    cJSON *network = cJSON_AddObjectToObject(response, "network");
    cJSON_AddBoolToObject(network, "connected", gw_status.wifi_connected);
    cJSON_AddStringToObject(network, "state", gw_status.wifi_state);
    cJSON_AddStringToObject(network, "ssid", gw_status.wifi_ssid);
    cJSON_AddStringToObject(network, "ip", gw_status.ip);
    cJSON_AddStringToObject(network, "mac", gw_status.wifi_mac);
    if (gw_status.has_wifi_rssi) {
        cJSON_AddNumberToObject(network, "rssi", gw_status.wifi_rssi);
    } else {
        cJSON_AddNullToObject(network, "rssi");
    }

    cJSON *auth_obj = cJSON_AddObjectToObject(response, "auth");
    cJSON_AddBoolToObject(auth_obj, "enabled", auth_status.enabled);
    cJSON_AddBoolToObject(auth_obj, "configured",
                          auth_status.credentials_configured);
    if (auth_status.username[0] != '\0') {
        cJSON_AddStringToObject(auth_obj, "username", auth_status.username);
    }

    cJSON *mcp = cJSON_AddObjectToObject(response, "mcp");
    cJSON_AddBoolToObject(mcp, "configured", mcp_token_configured);
    if (mcp_token_configured && mcp_token_preview[0] != '\0') {
        cJSON_AddStringToObject(mcp, "preview", mcp_token_preview);
    }

    return web_send_json(request, response);
}

esp_err_t web_settings_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {"/api/settings", HTTP_GET, settings_get_handler, NULL},
    };

    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", routes[i].uri,
                     esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "Settings API registered");
    return ESP_OK;
}
