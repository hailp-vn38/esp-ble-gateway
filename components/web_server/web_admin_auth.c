#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "web_admin_auth.h"
#include "web_http.h"

static const char *TAG = "web_admin_auth";
static const char *ADMIN_NAMESPACE = "web_admin";
static const char *ADMIN_KEY = "token";

static char s_config_token[128];
static bool s_initialized = false;

// Constant-time comparison to prevent timing side-channels.
static bool constant_time_compare(const char *a, const char *b, size_t len)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

esp_err_t web_admin_auth_init(void)
{
    if (s_initialized) return ESP_OK;

    // Try Kconfig first.
    const char *kconfig_token = CONFIG_WEB_ADMIN_AUTH_TOKEN;
    if (kconfig_token[0] != '\0') {
        strlcpy(s_config_token, kconfig_token, sizeof(s_config_token));
        s_initialized = true;
        ESP_LOGI(TAG, "Admin auth loaded from Kconfig");
        return ESP_OK;
    }

    // Try NVS.
    nvs_handle_t h;
    esp_err_t err = nvs_open(ADMIN_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = sizeof(s_config_token);
        err = nvs_get_str(h, ADMIN_KEY, s_config_token, &len);
        nvs_close(h);
        if (err == ESP_OK && s_config_token[0] != '\0') {
            s_initialized = true;
            ESP_LOGI(TAG, "Admin auth loaded from NVS");
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Admin token not configured — exposure API disabled");
    s_initialized = true;
    return ESP_OK;
}

esp_err_t web_admin_auth_set_token(const char *token)
{
    if (token == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(ADMIN_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, ADMIN_KEY, token);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        strlcpy(s_config_token, token, sizeof(s_config_token));
    }
    return err;
}

web_admin_auth_result_t web_admin_auth_check(httpd_req_t *req)
{
    // Fail closed if no token configured.
    if (s_config_token[0] == '\0') {
        return WEB_ADMIN_AUTH_NOT_CONFIGURED;
    }

    // Extract Bearer token.
    char auth_header[256];
    esp_err_t hdr_err = httpd_req_get_hdr_value_str(req, "Authorization",
                                                     auth_header,
                                                     sizeof(auth_header));
    if (hdr_err != ESP_OK) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        return WEB_ADMIN_AUTH_UNAUTHORIZED;
    }

    // Expect "Bearer <token>".
    const char *bearer_prefix = "Bearer ";
    if (strncmp(auth_header, bearer_prefix, strlen(bearer_prefix)) != 0) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        return WEB_ADMIN_AUTH_UNAUTHORIZED;
    }

    const char *token = auth_header + strlen(bearer_prefix);
    size_t token_len = strlen(token);
    size_t expected_len = strlen(s_config_token);

    if (token_len != expected_len ||
        !constant_time_compare(token, s_config_token, token_len)) {
        return WEB_ADMIN_AUTH_UNAUTHORIZED;
    }

    return WEB_ADMIN_AUTH_OK;
}
