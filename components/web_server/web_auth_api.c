#include "web_modules.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "web_auth.h"
#include "web_auth_http.h"
#include "web_http.h"

// ---------------------------------------------------------------------------
// POST /api/auth/login
// ---------------------------------------------------------------------------

static esp_err_t auth_login_handler(httpd_req_t *request)
{
    char body[256];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) {
        return web_send_body_error(request, body_status);
    }

    const char *username = web_get_json_string(json, "username", 33, true);
    const char *password = web_get_json_string(json, "password", 65, true);
    if (username == NULL || password == NULL) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "username and password required",
                                       "invalid_request");
    }

    char session_token[64];
    web_auth_result_t result = web_auth_login(username, password,
                                              session_token, sizeof(session_token));
    cJSON_Delete(json);

    if (result == WEB_AUTH_OK) {
        web_auth_set_session_cookie(
            request, session_token,
            web_auth_session_cookie_max_age_seconds());
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        cJSON *response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        return web_send_json(request, response);
    }

    if (result == WEB_AUTH_RATE_LIMITED) {
        return web_send_api_error_code(request, "429 Too Many Requests",
                                       "Too many login attempts",
                                       "auth_rate_limited");
    }

    if (result == WEB_AUTH_NOT_CONFIGURED) {
        return web_send_api_error_code(request, "409 Conflict",
                                       "Authentication is disabled",
                                       "auth_disabled");
    }

    return web_send_api_error_code(request, "401 Unauthorized",
                                   "Invalid username or password",
                                   "auth_invalid_credentials");
}

// ---------------------------------------------------------------------------
// POST /api/auth/logout
// ---------------------------------------------------------------------------

static esp_err_t auth_logout_handler(httpd_req_t *request)
{
    web_auth_result_t auth = web_auth_require_request(request);
    if (auth != WEB_AUTH_OK) {
        return web_send_api_error_code(request, "401 Unauthorized",
                                       "Authentication required",
                                       "auth_required");
    }

    char session_token[64] = {0};
    if (web_auth_get_session_token(request, session_token,
                                   sizeof(session_token)) == ESP_OK) {
        web_auth_logout(session_token);
    }
    web_auth_clear_session_cookie(request);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    return web_send_json(request, response);
}

// ---------------------------------------------------------------------------
// PUT /api/auth/config
// ---------------------------------------------------------------------------

static esp_err_t auth_config_handler(httpd_req_t *request)
{
    web_auth_result_t auth = web_auth_require_request(request);
    if (auth != WEB_AUTH_OK) {
        return web_send_api_error_code(request, "401 Unauthorized",
                                       "Authentication required",
                                       "auth_required");
    }

    char body[512];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) {
        return web_send_body_error(request, body_status);
    }

    const cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(json, "enabled");

    if (!cJSON_IsBool(enabled_item)) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "enabled must be boolean",
                                       "invalid_request");
    }

    bool enabled = cJSON_IsTrue(enabled_item);
    const char *username = web_get_json_string(json, "username", 33, false);
    const char *current_password = web_get_json_string(json, "current_password", 65, false);
    const char *new_password = web_get_json_string(json, "new_password", 65, false);

    web_auth_result_t result;

    if (enabled) {
        // Enable authentication
        if (new_password != NULL && username != NULL) {
            // First-time enable with new credentials
            result = web_auth_enable(username, NULL, new_password);
        } else if (current_password != NULL) {
            // Re-enable with existing credentials
            web_auth_status_t status;
            web_auth_get_status(&status);
            result = web_auth_enable(status.username, current_password, NULL);
        } else {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                           "Missing credentials",
                                           "invalid_request");
        }
    } else {
        // Disable authentication
        if (current_password == NULL) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                           "current_password required",
                                           "auth_current_password_required");
        }
        result = web_auth_disable(current_password);
    }

    cJSON_Delete(json);

    if (result == WEB_AUTH_OK) {
        web_auth_clear_session_cookie(request);
        cJSON *response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddBoolToObject(response, "reauth_required", enabled);
        return web_send_json(request, response);
    }

    if (result == WEB_AUTH_CURRENT_PASSWORD_INVALID) {
        return web_send_api_error_code(request, "403 Forbidden",
                                       "Current password is incorrect",
                                       "auth_current_password_invalid");
    }

    if (result == WEB_AUTH_INVALID_USERNAME) {
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Invalid username format",
                                       "auth_username_invalid");
    }

    if (result == WEB_AUTH_INVALID_PASSWORD) {
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Password must be 8-64 characters",
                                       "auth_password_invalid");
    }

    return web_send_api_error_code(request, "500 Internal Server Error",
                                   "Failed to update authentication",
                                   "auth_storage_error");
}

// ---------------------------------------------------------------------------
// PUT /api/auth/password
// ---------------------------------------------------------------------------

static esp_err_t auth_password_handler(httpd_req_t *request)
{
    web_auth_result_t auth = web_auth_require_request(request);
    if (auth != WEB_AUTH_OK) {
        return web_send_api_error_code(request, "401 Unauthorized",
                                       "Authentication required",
                                       "auth_required");
    }

    char body[512];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) {
        return web_send_body_error(request, body_status);
    }

    const char *current_password = web_get_json_string(json, "current_password", 65, true);
    const char *new_password = web_get_json_string(json, "new_password", 65, true);
    if (current_password == NULL || new_password == NULL) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "current_password and new_password required",
                                       "invalid_request");
    }

    web_auth_result_t result = web_auth_change_password(current_password, new_password);
    cJSON_Delete(json);

    if (result == WEB_AUTH_OK) {
        // Clear current session cookie
        web_auth_clear_session_cookie(request);

        cJSON *response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddBoolToObject(response, "reauth_required", true);
        return web_send_json(request, response);
    }

    if (result == WEB_AUTH_CURRENT_PASSWORD_INVALID) {
        return web_send_api_error_code(request, "403 Forbidden",
                                       "Current password is incorrect",
                                       "auth_current_password_invalid");
    }

    if (result == WEB_AUTH_INVALID_PASSWORD) {
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Password must be 8-64 characters",
                                       "auth_password_invalid");
    }

    return web_send_api_error_code(request, "500 Internal Server Error",
                                   "Failed to change password",
                                   "auth_storage_error");
}

// ---------------------------------------------------------------------------
// Auth API route registration
// ---------------------------------------------------------------------------

esp_err_t web_auth_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/auth/login", .method = HTTP_POST,
         .handler = auth_login_handler},
        {.uri = "/api/auth/logout", .method = HTTP_POST,
         .handler = auth_logout_handler},
        {.uri = "/api/auth/config", .method = HTTP_PUT,
         .handler = auth_config_handler},
        {.uri = "/api/auth/password", .method = HTTP_PUT,
         .handler = auth_password_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
