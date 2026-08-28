#ifndef WEB_AUTH_H
#define WEB_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    bool credentials_configured;
    char username[33];
} web_auth_status_t;

typedef enum {
    WEB_AUTH_OK = 0,
    WEB_AUTH_REQUIRED,
    WEB_AUTH_INVALID_CREDENTIALS,
    WEB_AUTH_RATE_LIMITED,
    WEB_AUTH_NOT_CONFIGURED,
    WEB_AUTH_STORAGE_ERROR,
    WEB_AUTH_INVALID_USERNAME,
    WEB_AUTH_INVALID_PASSWORD,
    WEB_AUTH_CURRENT_PASSWORD_REQUIRED,
    WEB_AUTH_CURRENT_PASSWORD_INVALID,
} web_auth_result_t;

esp_err_t web_auth_init(void);
esp_err_t web_auth_get_status(web_auth_status_t *out);

web_auth_result_t web_auth_login(const char *username,
                                 const char *password,
                                 char *session_token,
                                 size_t session_token_size);

web_auth_result_t web_auth_validate_session(const char *session_token);
void web_auth_logout(const char *session_token);
void web_auth_invalidate_all_sessions(void);

web_auth_result_t web_auth_enable(const char *username,
                                  const char *current_password,
                                  const char *new_password);

web_auth_result_t web_auth_disable(const char *current_password);

web_auth_result_t web_auth_change_username(const char *current_password,
                                           const char *new_username);

web_auth_result_t web_auth_change_password(const char *current_password,
                                           const char *new_password);

#endif // WEB_AUTH_H
