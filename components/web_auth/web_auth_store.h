#ifndef WEB_AUTH_STORE_H
#define WEB_AUTH_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t web_auth_store_load(bool *enabled, char *username, size_t username_size,
                              uint8_t *pwd_salt, uint8_t *pwd_hash, uint32_t *pwd_iter);
esp_err_t web_auth_store_save(bool enabled, const char *username,
                              const uint8_t *pwd_salt, const uint8_t *pwd_hash,
                              uint32_t pwd_iter);
esp_err_t web_auth_store_set_enabled(bool enabled);
bool web_auth_store_has_credentials(void);
esp_err_t web_auth_store_erase(void);

#endif // WEB_AUTH_STORE_H
