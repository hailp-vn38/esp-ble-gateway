#ifndef WEB_AUTH_SESSION_H
#define WEB_AUTH_SESSION_H

#include <stdint.h>

#include "esp_err.h"
#include "web_auth.h"

#define WEB_AUTH_SESSION_TOKEN_LENGTH 43
#define WEB_AUTH_SESSION_TOKEN_BUFFER_SIZE (WEB_AUTH_SESSION_TOKEN_LENGTH + 1)

esp_err_t web_auth_session_init(void);
esp_err_t web_auth_session_create(char *token_out, size_t token_out_size);
web_auth_result_t web_auth_session_validate(const char *token);
void web_auth_session_destroy(const char *token);
void web_auth_session_destroy_all(void);
void web_auth_session_cleanup_expired(void);

#endif // WEB_AUTH_SESSION_H
