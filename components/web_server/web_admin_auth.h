#ifndef WEB_ADMIN_AUTH_H
#define WEB_ADMIN_AUTH_H

#include "esp_err.h"
#include "esp_http_server.h"

typedef enum {
    WEB_ADMIN_AUTH_OK = 0,
    WEB_ADMIN_AUTH_NOT_CONFIGURED,
    WEB_ADMIN_AUTH_UNAUTHORIZED,
    WEB_ADMIN_AUTH_FORBIDDEN_HOST,
} web_admin_auth_result_t;

esp_err_t web_admin_auth_init(void);

web_admin_auth_result_t web_admin_auth_check(httpd_req_t *req);

esp_err_t web_admin_auth_set_token(const char *token);

#endif /* WEB_ADMIN_AUTH_H */
