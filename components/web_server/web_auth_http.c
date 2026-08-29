#include "web_auth.h"

#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"

static const char *SESSION_COOKIE_NAME = "GWSESSION";

static esp_err_t extract_cookie_value(httpd_req_t *req, const char *name,
                                      char *value, size_t value_size)
{
    size_t buf_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (buf_len == 0) return ESP_ERR_NOT_FOUND;

    char *buf = malloc(buf_len + 1);
    if (buf == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = httpd_req_get_hdr_value_str(req, "Cookie", buf, buf_len + 1);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    // Parse cookie header: name=value; name2=value2
    char *saveptr = NULL;
    char *token = strtok_r(buf, ";", &saveptr);
    while (token != NULL) {
        // Skip leading whitespace
        while (*token == ' ') token++;

        size_t name_len = strlen(name);
        if (strncmp(token, name, name_len) == 0 && token[name_len] == '=') {
            const char *val = token + name_len + 1;
            if (strlen(val) >= value_size) {
                free(buf);
                return ESP_ERR_INVALID_SIZE;
            }
            strlcpy(value, val, value_size);
            free(buf);
            return ESP_OK;
        }
        token = strtok_r(NULL, ";", &saveptr);
    }

    free(buf);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t web_auth_get_session_token(httpd_req_t *req, char *token,
                                     size_t token_size)
{
    if (req == NULL || token == NULL || token_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    token[0] = '\0';
    return extract_cookie_value(req, SESSION_COOKIE_NAME, token, token_size);
}

web_auth_result_t web_auth_require_request(httpd_req_t *req)
{
    // Check if auth is enabled
    web_auth_status_t status;
    esp_err_t err = web_auth_get_status(&status);
    if (err != ESP_OK) return WEB_AUTH_STORAGE_ERROR;
    if (!status.enabled) return WEB_AUTH_OK;

    // Extract session cookie
    char session_token[64] = {0};
    err = web_auth_get_session_token(req, session_token,
                                     sizeof(session_token));
    if (err != ESP_OK || session_token[0] == '\0') {
        return WEB_AUTH_REQUIRED;
    }

    // Validate session
    return web_auth_validate_session(session_token);
}

void web_auth_set_session_cookie(httpd_req_t *req, const char *token,
                                 int max_age_seconds)
{
    char cookie[128];
    snprintf(cookie, sizeof(cookie),
             "%s=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%d",
             SESSION_COOKIE_NAME, token, max_age_seconds);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

void web_auth_clear_session_cookie(httpd_req_t *req)
{
    char cookie[128];
    snprintf(cookie, sizeof(cookie),
             "%s=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0",
             SESSION_COOKIE_NAME);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}
