#include "web_modules.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "gateway_status.h"
#include "mcp_ws_bridge.h"

#include "web_http.h"

static const char *TAG = "web_settings_api";

static void zeroize_endpoint_json(cJSON *endpoint_item, char *body,
                                  size_t body_size)
{
    if (cJSON_IsString(endpoint_item) && endpoint_item->valuestring != NULL) {
        memset(endpoint_item->valuestring, 0, strlen(endpoint_item->valuestring));
    }
    memset(body, 0, body_size);
}

static void add_xiaozhi_status(cJSON *response)
{
    mcp_ws_public_config_t config = {0};
    mcp_ws_status_t status = {0};
    esp_err_t config_result = mcp_ws_bridge_config_get_public(&config);
    esp_err_t status_result = mcp_ws_bridge_get_status(&status);
    cJSON *xiaozhi = cJSON_AddObjectToObject(response, "xiaozhi");
    cJSON_AddBoolToObject(xiaozhi, "supported", mcp_ws_bridge_is_supported());
    cJSON_AddBoolToObject(xiaozhi, "enabled",
                          config_result == ESP_OK && config.enabled);
    if (status_result == ESP_OK) {
        cJSON_AddBoolToObject(xiaozhi, "runtime_enabled", status.runtime_enabled);
        cJSON_AddBoolToObject(xiaozhi, "restart_required",
                              status.restart_required);
    } else {
        cJSON_AddBoolToObject(xiaozhi, "runtime_enabled", false);
        cJSON_AddBoolToObject(xiaozhi, "restart_required", false);
    }
    cJSON_AddBoolToObject(xiaozhi, "endpoint_configured",
                          config_result == ESP_OK && config.endpoint_configured);
    if (config_result == ESP_OK && config.endpoint_display[0] != '\0') {
        cJSON_AddStringToObject(xiaozhi, "endpoint_display",
                                config.endpoint_display);
    }
    cJSON_AddStringToObject(
        xiaozhi, "state",
        status_result == ESP_OK ? mcp_ws_bridge_state_name(status.state)
                                : "unsupported");
    cJSON_AddNumberToObject(xiaozhi, "retry_count",
                            status_result == ESP_OK ? status.retry_count : 0);
    cJSON_AddNumberToObject(xiaozhi, "last_error",
                            status_result == ESP_OK ? status.last_error : 0);
    cJSON_AddNumberToObject(
        xiaozhi, "last_http_status",
        status_result == ESP_OK ? status.last_http_status : 0);
    cJSON_AddNumberToObject(
        xiaozhi, "last_ws_close_code",
        status_result == ESP_OK ? status.last_ws_close_code : 0);
    if (status_result == ESP_OK &&
        status.negotiated_protocol_version[0] != '\0') {
        cJSON_AddStringToObject(xiaozhi, "protocol_version",
                                status.negotiated_protocol_version);
    }
}

static esp_err_t settings_get_handler(httpd_req_t *request)
{
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
    cJSON_AddBoolToObject(auth_obj, "enabled", false);
    cJSON_AddBoolToObject(auth_obj, "configured", false);

    cJSON *mcp = cJSON_AddObjectToObject(response, "mcp");
    cJSON_AddBoolToObject(mcp, "configured", mcp_token_configured);
    if (mcp_token_configured && mcp_token_preview[0] != '\0') {
        cJSON_AddStringToObject(mcp, "preview", mcp_token_preview);
    }

    add_xiaozhi_status(response);

    return web_send_json(request, response);
}

static esp_err_t xiaozhi_put_handler(httpd_req_t *request)
{
    if (!mcp_ws_bridge_is_supported()) {
        return web_send_api_error_code(request, "501 Not Implemented",
                                       "Xiaozhi is not supported by this firmware",
                                       "xiaozhi_unsupported");
    }

    char body[MCP_WS_ENDPOINT_MAX_LEN + 160];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) return web_send_body_error(request, body_status);

    const cJSON *enabled_item =
        cJSON_GetObjectItemCaseSensitive(json, "enabled");
    const cJSON *endpoint_item =
        cJSON_GetObjectItemCaseSensitive(json, "endpoint");
    const cJSON *clear_item =
        cJSON_GetObjectItemCaseSensitive(json, "clear_endpoint");
    bool has_enabled = enabled_item != NULL;
    bool has_endpoint = endpoint_item != NULL;
    bool clear_endpoint = cJSON_IsTrue(clear_item);

    if ((has_enabled && !cJSON_IsBool(enabled_item)) ||
        (has_endpoint && (!cJSON_IsString(endpoint_item) ||
                          endpoint_item->valuestring == NULL ||
                          strnlen(endpoint_item->valuestring,
                                  MCP_WS_ENDPOINT_MAX_LEN) >=
                              MCP_WS_ENDPOINT_MAX_LEN)) ||
        (clear_item != NULL && !cJSON_IsBool(clear_item)) ||
        (has_endpoint && clear_endpoint) ||
        (!has_enabled && !has_endpoint && !clear_endpoint)) {
        zeroize_endpoint_json((cJSON *)endpoint_item, body, sizeof(body));
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Invalid Xiaozhi settings",
                                       "invalid_request");
    }

    const char *endpoint = clear_endpoint ? "" :
        (has_endpoint ? endpoint_item->valuestring : NULL);
    esp_err_t result = mcp_ws_bridge_config_update(
        has_enabled, cJSON_IsTrue(enabled_item),
        has_endpoint || clear_endpoint, endpoint);
    zeroize_endpoint_json((cJSON *)endpoint_item, body, sizeof(body));
    cJSON_Delete(json);
    if (result == ESP_ERR_INVALID_ARG) {
        return web_send_api_error_code(
            request, "400 Bad Request",
            "Endpoint must be a valid wss:// URL and is required when enabled",
            "invalid_endpoint");
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Xiaozhi settings: %s",
                 esp_err_to_name(result));
        return web_send_api_error_code(request, "500 Internal Server Error",
                                       "Could not save Xiaozhi settings",
                                       "internal_error");
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    add_xiaozhi_status(response);
    return web_send_json(request, response);
}

static esp_err_t xiaozhi_reconnect_handler(httpd_req_t *request)
{
    if (!mcp_ws_bridge_is_supported()) {
        return web_send_api_error_code(request, "501 Not Implemented",
                                       "Xiaozhi is not supported by this firmware",
                                       "xiaozhi_unsupported");
    }

    mcp_ws_status_t status = {0};
    esp_err_t result = mcp_ws_bridge_get_status(&status);
    if (result != ESP_OK) {
        return web_send_api_error_code(request, "500 Internal Server Error",
                                       "Could not read Xiaozhi status",
                                       "internal_error");
    }

    if (!status.runtime_enabled) {
        if (status.restart_required) {
            return web_send_api_error_code(
                request, "409 Conflict",
                "Gateway restart is required before reconnecting Xiaozhi",
                "restart_required");
        }
        return web_send_api_error_code(request, "409 Conflict",
                                       "Xiaozhi is disabled",
                                       "xiaozhi_disabled");
    }

    if (!status.endpoint_configured) {
        return web_send_api_error_code(
            request, "409 Conflict",
            "Xiaozhi endpoint is not configured",
            "xiaozhi_not_configured");
    }

    if (status.state == MCP_WS_CONNECTING ||
        status.state == MCP_WS_HANDSHAKING) {
        return web_send_api_error_code(request, "409 Conflict",
                                       "Xiaozhi is currently connecting",
                                       "xiaozhi_busy");
    }

    result = mcp_ws_bridge_reload();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Xiaozhi reconnect failed: %s", esp_err_to_name(result));
        return web_send_api_error_code(request, "500 Internal Server Error",
                                       "Xiaozhi reconnect failed",
                                       "internal_error");
    }

    mcp_ws_status_t new_status = {0};
    mcp_ws_bridge_get_status(&new_status);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "state",
                            mcp_ws_bridge_state_name(new_status.state));
    return web_send_json(request, response);
}

esp_err_t web_settings_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        WEB_URI_INIT("/api/settings", HTTP_GET, settings_get_handler),
        WEB_URI_INIT("/api/settings/xiaozhi", HTTP_PUT, xiaozhi_put_handler),
        WEB_URI_INIT("/api/settings/xiaozhi/reconnect", HTTP_POST, xiaozhi_reconnect_handler),
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
