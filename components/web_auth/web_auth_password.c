#include "web_auth.h"

#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"
#include "esp_random.h"

static const char *TAG = "web_auth_password";

#define PBKDF2_SALT_LEN 16
#define PBKDF2_HASH_LEN 32

extern int web_auth_get_pbkdf2_iterations(void);

static esp_err_t hmac_sha256(const uint8_t *key, size_t key_len,
                              const uint8_t *msg, size_t msg_len,
                              uint8_t *out, size_t out_size, size_t *out_len)
{
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_bits(&attrs, 256);

    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t s = psa_import_key(&attrs, key, key_len, &key_id);
    if (s != PSA_SUCCESS) return ESP_FAIL;

    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    s = psa_mac_sign_setup(&op, key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    if (s != PSA_SUCCESS) { psa_destroy_key(key_id); return ESP_FAIL; }

    s = psa_mac_update(&op, msg, msg_len);
    if (s != PSA_SUCCESS) { psa_mac_abort(&op); psa_destroy_key(key_id); return ESP_FAIL; }

    s = psa_mac_sign_finish(&op, out, out_size, out_len);
    psa_destroy_key(key_id);
    return (s == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t pbkdf2_hmac_sha256(const char *password,
                                     const uint8_t *salt, size_t salt_len,
                                     uint32_t iterations,
                                     size_t key_len, uint8_t *key)
{
    size_t hash_len = 32;
    size_t num_blocks = (key_len + hash_len - 1) / hash_len;

    for (size_t block = 1; block <= num_blocks; block++) {
        uint8_t U[32];
        uint8_t T[32];

        uint8_t salt_block[salt_len + 4];
        memcpy(salt_block, salt, salt_len);
        salt_block[salt_len]     = (block >> 24) & 0xFF;
        salt_block[salt_len + 1] = (block >> 16) & 0xFF;
        salt_block[salt_len + 2] = (block >> 8)  & 0xFF;
        salt_block[salt_len + 3] =  block        & 0xFF;

        size_t mac_len = 0;
        esp_err_t err = hmac_sha256((const uint8_t *)password, strlen(password),
                                     salt_block, sizeof(salt_block),
                                     U, sizeof(U), &mac_len);
        if (err != ESP_OK) return err;

        memcpy(T, U, hash_len);

        for (uint32_t i = 1; i < iterations; i++) {
            err = hmac_sha256((const uint8_t *)password, strlen(password),
                              U, hash_len, U, sizeof(U), &mac_len);
            if (err != ESP_OK) return err;

            for (size_t j = 0; j < hash_len; j++) {
                T[j] ^= U[j];
            }
        }

        size_t offset = (block - 1) * hash_len;
        size_t copy_len = (key_len - offset > hash_len) ? hash_len : (key_len - offset);
        memcpy(key + offset, T, copy_len);
    }

    return ESP_OK;
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

    esp_err_t err = pbkdf2_hmac_sha256(password, salt, salt_len,
                                        *iterations, hash_len, hash);
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

    esp_err_t err = pbkdf2_hmac_sha256(password, salt, salt_len,
                                        iterations, hash_len, computed_hash);
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
