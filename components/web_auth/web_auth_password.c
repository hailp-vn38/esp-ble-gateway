#include "web_auth.h"
#include "web_auth_password.h"

#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"
#include "esp_random.h"

static const char *TAG = "web_auth_password";

#define PBKDF2_HASH_LEN 32
#define PBKDF2_MIN_ITERATIONS 10000U
#define PBKDF2_MAX_ITERATIONS 200000U

extern int web_auth_get_pbkdf2_iterations(void);

esp_err_t web_auth_password_derive(const char *password,
                                   const uint8_t *salt, size_t salt_len,
                                   uint32_t iterations,
                                   uint8_t *output, size_t output_len)
{
    if (password == NULL || salt == NULL || salt_len == 0 || output == NULL ||
        output_len == 0 || iterations < PBKDF2_MIN_ITERATIONS ||
        iterations > PBKDF2_MAX_ITERATIONS) {
        return ESP_ERR_INVALID_ARG;
    }

    psa_key_derivation_operation_t operation =
        PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(
        &operation, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_integer(
            &operation, PSA_KEY_DERIVATION_INPUT_COST, iterations);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(
            &operation, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(
            &operation, PSA_KEY_DERIVATION_INPUT_PASSWORD,
            (const uint8_t *)password, strlen(password));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&operation, output,
                                                 output_len);
    }
    psa_key_derivation_abort(&operation);
    return status == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}

esp_err_t web_auth_password_hash(const char *password,
                                 uint8_t *salt, size_t salt_len,
                                 uint8_t *hash, size_t hash_len,
                                 uint32_t *iterations)
{
    if (password == NULL || salt == NULL || hash == NULL || iterations == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_fill_random(salt, salt_len);
    *iterations = web_auth_get_pbkdf2_iterations();

    esp_err_t err = web_auth_password_derive(password, salt, salt_len,
                                             *iterations, hash, hash_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PBKDF2 failed");
        return err;
    }

    return ESP_OK;
}

esp_err_t web_auth_password_verify(const char *password,
                                   const uint8_t *salt, size_t salt_len,
                                   const uint8_t *expected_hash, size_t hash_len,
                                   uint32_t iterations)
{
    if (password == NULL || salt == NULL || expected_hash == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t computed_hash[PBKDF2_HASH_LEN];
    if (hash_len > sizeof(computed_hash)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = web_auth_password_derive(password, salt, salt_len,
                                             iterations, computed_hash,
                                             hash_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PBKDF2 verify failed");
        return err;
    }

    volatile uint8_t diff = 0;
    for (size_t i = 0; i < hash_len; i++) {
        diff |= computed_hash[i] ^ expected_hash[i];
    }

    return diff == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool web_auth_password_validate(const char *password)
{
    if (password == NULL) return false;
    size_t len = strlen(password);
    return len >= 8 && len <= 64;
}

bool web_auth_username_validate(const char *username)
{
    if (username == NULL) return false;
    size_t len = strlen(username);
    if (len < 3 || len > 32) return false;

    for (size_t i = 0; i < len; i++) {
        char c = username[i];
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}
