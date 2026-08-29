#ifndef WEB_AUTH_PASSWORD_H
#define WEB_AUTH_PASSWORD_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t web_auth_password_hash(const char *password,
                                 uint8_t *salt, size_t salt_len,
                                 uint8_t *hash, size_t hash_len,
                                 uint32_t *iterations);
esp_err_t web_auth_password_verify(const char *password,
                                   const uint8_t *salt, size_t salt_len,
                                   const uint8_t *expected_hash,
                                   size_t hash_len, uint32_t iterations);

/* Deterministic entry point for PBKDF2 vector tests. */
esp_err_t web_auth_password_derive(const char *password,
                                   const uint8_t *salt, size_t salt_len,
                                   uint32_t iterations,
                                   uint8_t *output, size_t output_len);

#endif // WEB_AUTH_PASSWORD_H
