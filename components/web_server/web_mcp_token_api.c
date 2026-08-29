#include "web_modules.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include "web_auth.h"
#include "web_auth_http.h"
#include "web_http.h"

static const char *TAG = "web_mcp_token_api";
static const char *NVS_NAMESPACE = "mcp";
static const char *NVS_TOKEN_KEY = "token";

#define MCP_TOKEN_RANDOM_BYTES 16
#define MCP_TOKEN_PREFIX       "mcp_"
#define MCP_TOKEN_LENGTH       (sizeof(MCP_TOKEN_PREFIX) - 1 + \
                                MCP_TOKEN_RANDOM_BYTES * 2)

#ifndef CONFIG_MCP_AUTH_TOKEN
#define CONFIG_MCP_AUTH_TOKEN ""
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static esp_err_t nvs_get_token(char *token, size_t max_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = max_len;
    err = nvs_get_str(handle, NVS_TOKEN_KEY, token, &len);
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_set_token(const char *token)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, NVS_TOKEN_KEY, token);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void generate_random_token(char token[MCP_TOKEN_LENGTH + 1])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random[MCP_TOKEN_RANDOM_BYTES];
    esp_fill_random(random, sizeof(random));

    memcpy(token, MCP_TOKEN_PREFIX, sizeof(MCP_TOKEN_PREFIX) - 1);
    size_t output = sizeof(MCP_TOKEN_PREFIX) - 1;
    for (size_t i = 0; i < sizeof(random); i++) {
        token[output++] = hex[random[i] >> 4];
        token[output++] = hex[random[i] & 0x0f];
    }
    token[output] = '\0';
}

esp_err_t web_mcp_token_get_status(bool *configured, char *preview,
                                   size_t preview_size)
{
    if (configured == NULL || preview == NULL || preview_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char token[128] = {0};
    esp_err_t err = nvs_get_token(token, sizeof(token));
    const char *effective_token = NULL;
    if (err == ESP_OK && token[0] != '\0') {
        effective_token = token;
    } else if (CONFIG_MCP_AUTH_TOKEN[0] != '\0') {
        effective_token = CONFIG_MCP_AUTH_TOKEN;
    }

    *configured = effective_token != NULL;
    preview[0] = '\0';
    if (effective_token != NULL) {
        size_t length = strlen(effective_token);
        const char *suffix = length > 4 ? effective_token + length - 4
                                        : effective_token;
        snprintf(preview, preview_size, "...%s", suffix);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /api/mcp-token/generate — Generate new MCP token (Web Auth protected)
// ---------------------------------------------------------------------------

static esp_err_t mcp_token_generate_handler(httpd_req_t *request)
{
    web_auth_result_t auth = web_auth_require_request(request);
    if (auth != WEB_AUTH_OK) {
        return web_send_api_error_code(request, "401 Unauthorized",
                                       "Authentication required",
                                       "auth_required");
    }

    char new_token[MCP_TOKEN_LENGTH + 1];
    generate_random_token(new_token);

    esp_err_t err = nvs_set_token(new_token);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save MCP token: %s", esp_err_to_name(err));
        return web_send_api_error_code(request, "500 Internal Server Error",
                                       "Failed to save token",
                                       "internal_error");
    }

    ESP_LOGI(TAG, "MCP token generated");

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "token", new_token);
    return web_send_json(request, response);
}

// ---------------------------------------------------------------------------
// PUT /api/mcp-token — Update MCP token (Web Auth protected)
// ---------------------------------------------------------------------------

static esp_err_t mcp_token_update_handler(httpd_req_t *request)
{
    web_auth_result_t auth = web_auth_require_request(request);
    if (auth != WEB_AUTH_OK) {
        return web_send_api_error_code(request, "401 Unauthorized",
                                       "Authentication required",
                                       "auth_required");
    }

    char body[256];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) {
        return web_send_body_error(request, body_status);
    }

    const char *token = web_get_json_string(json, "token", 128, true);
    if (token == NULL || strlen(token) < 8) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Token must be at least 8 characters",
                                       "invalid_request");
    }

    esp_err_t err = nvs_set_token(token);
    cJSON_Delete(json);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save MCP token: %s", esp_err_to_name(err));
        return web_send_api_error_code(request, "500 Internal Server Error",
                                       "Failed to save token",
                                       "internal_error");
    }

    ESP_LOGI(TAG, "MCP token updated");

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    return web_send_json(request, response);
}

// ---------------------------------------------------------------------------
// DELETE /api/mcp-token — Revoke MCP token (Web Auth protected)
// ---------------------------------------------------------------------------

static esp_err_t mcp_token_delete_handler(httpd_req_t *request)
{
    web_auth_result_t auth = web_auth_require_request(request);
    if (auth != WEB_AUTH_OK) {
        return web_send_api_error_code(request, "401 Unauthorized",
                                       "Authentication required",
                                       "auth_required");
    }

    esp_err_t err = nvs_set_token("");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear MCP token: %s", esp_err_to_name(err));
        return web_send_api_error_code(request, "500 Internal Server Error",
                                       "Failed to clear token",
                                       "internal_error");
    }

    ESP_LOGI(TAG, "MCP token revoked");

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    return web_send_json(request, response);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

esp_err_t web_mcp_token_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {"/api/mcp-token", HTTP_PUT, mcp_token_update_handler, NULL},
        {"/api/mcp-token", HTTP_DELETE, mcp_token_delete_handler, NULL},
        {"/api/mcp-token/generate", HTTP_POST, mcp_token_generate_handler, NULL},
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

    ESP_LOGI(TAG, "MCP token API registered");
    return ESP_OK;
}
