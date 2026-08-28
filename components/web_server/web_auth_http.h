#ifndef WEB_AUTH_HTTP_H
#define WEB_AUTH_HTTP_H

#include "esp_err.h"
#include "esp_http_server.h"
#include "web_auth.h"

/**
 * Check if request is authorized.
 * Returns WEB_AUTH_OK if auth is disabled or session is valid.
 * Returns WEB_AUTH_REQUIRED if session is missing/invalid.
 */
web_auth_result_t web_auth_require_request(httpd_req_t *req);

/**
 * Set session cookie on response.
 */
void web_auth_set_session_cookie(httpd_req_t *req, const char *token,
                                 int max_age_seconds);

/**
 * Clear session cookie (for logout).
 */
void web_auth_clear_session_cookie(httpd_req_t *req);

/**
 * Extract cookie value from request headers.
 */
esp_err_t extract_cookie_value(httpd_req_t *req, const char *name,
                               char *value, size_t value_size);

#endif // WEB_AUTH_HTTP_H
