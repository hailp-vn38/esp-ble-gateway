#include "web_auth.h"
#include "web_auth_session.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "web_auth_session";

#define SESSION_RANDOM_BYTES 32

typedef struct {
    bool active;
    uint8_t token_hash[32];
    int64_t created_us;
    int64_t last_seen_us;
} session_entry_t;

static session_entry_t sessions[CONFIG_WEB_AUTH_MAX_SESSIONS];
static SemaphoreHandle_t session_mutex = NULL;

static int64_t get_time_us(void)
{
    return esp_timer_get_time();
}

static esp_err_t sha256_hash(const uint8_t *data, size_t len, uint8_t *hash)
{
    size_t out_len = 0;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, data, len, hash,
                                           32, &out_len);
    return status == PSA_SUCCESS && out_len == 32 ? ESP_OK : ESP_FAIL;
}

static bool token_has_canonical_shape(const char *token)
{
    if (token == NULL || strlen(token) != WEB_AUTH_SESSION_TOKEN_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < WEB_AUTH_SESSION_TOKEN_LENGTH; i++) {
        char c = token[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

static esp_err_t token_hash(const char *token, uint8_t hash[32])
{
    if (!token_has_canonical_shape(token)) return ESP_ERR_INVALID_ARG;
    return sha256_hash((const uint8_t *)token,
                       WEB_AUTH_SESSION_TOKEN_LENGTH, hash);
}

static bool hash_equal(const uint8_t left[32], const uint8_t right[32])
{
    volatile uint8_t difference = 0;
    for (size_t i = 0; i < 32; i++) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

esp_err_t web_auth_session_init(void)
{
    if (session_mutex != NULL) return ESP_OK;

    memset(sessions, 0, sizeof(sessions));
    session_mutex = xSemaphoreCreateMutex();
    return session_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t web_auth_session_create(char *token_out, size_t token_out_size)
{
    if (token_out == NULL ||
        token_out_size < WEB_AUTH_SESSION_TOKEN_BUFFER_SIZE ||
        session_mutex == NULL) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t random[SESSION_RANDOM_BYTES];
    esp_fill_random(random, sizeof(random));

    unsigned char base64[45] = {0};
    size_t base64_length = 0;
    int encoded = mbedtls_base64_encode(base64, sizeof(base64),
                                        &base64_length, random,
                                        sizeof(random));
    if (encoded != 0 || base64_length != 44 || base64[43] != '=') {
        return ESP_FAIL;
    }
    for (size_t i = 0; i < WEB_AUTH_SESSION_TOKEN_LENGTH; i++) {
        token_out[i] = base64[i] == '+' ? '-'
                     : base64[i] == '/' ? '_'
                                        : (char)base64[i];
    }
    token_out[WEB_AUTH_SESSION_TOKEN_LENGTH] = '\0';

    uint8_t hash[32];
    if (token_hash(token_out, hash) != ESP_OK) return ESP_FAIL;

    xSemaphoreTake(session_mutex, portMAX_DELAY);

    int slot = -1;
    for (int s = 0; s < CONFIG_WEB_AUTH_MAX_SESSIONS; s++) {
        if (!sessions[s].active) {
            slot = s;
            break;
        }
    }

    if (slot < 0) {
        int64_t oldest = INT64_MAX;
        slot = 0;
        for (int s = 0; s < CONFIG_WEB_AUTH_MAX_SESSIONS; s++) {
            if (sessions[s].last_seen_us < oldest) {
                oldest = sessions[s].last_seen_us;
                slot = s;
            }
        }
    }

    sessions[slot].active = true;
    memcpy(sessions[slot].token_hash, hash, sizeof(hash));
    sessions[slot].created_us = get_time_us();
    sessions[slot].last_seen_us = sessions[slot].created_us;

    xSemaphoreGive(session_mutex);

    ESP_LOGI(TAG, "Session created in slot %d", slot);
    return ESP_OK;
}

web_auth_result_t web_auth_session_validate(const char *token)
{
    if (token == NULL || session_mutex == NULL) {
        return WEB_AUTH_INVALID_CREDENTIALS;
    }

    uint8_t hash[32];
    if (token_hash(token, hash) != ESP_OK) {
        return WEB_AUTH_INVALID_CREDENTIALS;
    }

    int64_t now = get_time_us();
    int64_t idle_timeout = (int64_t)CONFIG_WEB_AUTH_SESSION_IDLE_MINUTES * 60 * 1000000LL;
    int64_t max_timeout = (int64_t)CONFIG_WEB_AUTH_SESSION_MAX_HOURS * 3600 * 1000000LL;

    xSemaphoreTake(session_mutex, portMAX_DELAY);

    for (int s = 0; s < CONFIG_WEB_AUTH_MAX_SESSIONS; s++) {
        if (!sessions[s].active) continue;

        if (hash_equal(sessions[s].token_hash, hash)) {
            if (now - sessions[s].created_us > max_timeout) {
                sessions[s].active = false;
                xSemaphoreGive(session_mutex);
                ESP_LOGI(TAG, "Session expired (absolute timeout)");
                return WEB_AUTH_INVALID_CREDENTIALS;
            }

            if (now - sessions[s].last_seen_us > idle_timeout) {
                sessions[s].active = false;
                xSemaphoreGive(session_mutex);
                ESP_LOGI(TAG, "Session expired (idle timeout)");
                return WEB_AUTH_INVALID_CREDENTIALS;
            }

            sessions[s].last_seen_us = now;
            xSemaphoreGive(session_mutex);
            return WEB_AUTH_OK;
        }
    }

    xSemaphoreGive(session_mutex);
    return WEB_AUTH_INVALID_CREDENTIALS;
}

void web_auth_session_destroy(const char *token)
{
    if (token == NULL || session_mutex == NULL) return;

    uint8_t hash[32];
    if (token_hash(token, hash) != ESP_OK) return;

    xSemaphoreTake(session_mutex, portMAX_DELAY);
    for (int s = 0; s < CONFIG_WEB_AUTH_MAX_SESSIONS; s++) {
        if (sessions[s].active &&
            hash_equal(sessions[s].token_hash, hash)) {
            memset(&sessions[s], 0, sizeof(sessions[s]));
            break;
        }
    }
    xSemaphoreGive(session_mutex);
    ESP_LOGI(TAG, "Session logged out");
}

void web_auth_session_destroy_all(void)
{
    if (session_mutex == NULL) return;

    xSemaphoreTake(session_mutex, portMAX_DELAY);
    memset(sessions, 0, sizeof(sessions));
    xSemaphoreGive(session_mutex);

    ESP_LOGI(TAG, "All sessions invalidated");
}

void web_auth_session_cleanup_expired(void)
{
    if (session_mutex == NULL) return;

    int64_t now = get_time_us();
    int64_t idle_timeout = (int64_t)CONFIG_WEB_AUTH_SESSION_IDLE_MINUTES * 60 * 1000000LL;
    int64_t max_timeout = (int64_t)CONFIG_WEB_AUTH_SESSION_MAX_HOURS * 3600 * 1000000LL;

    xSemaphoreTake(session_mutex, portMAX_DELAY);

    for (int s = 0; s < CONFIG_WEB_AUTH_MAX_SESSIONS; s++) {
        if (!sessions[s].active) continue;

        if (now - sessions[s].created_us > max_timeout ||
            now - sessions[s].last_seen_us > idle_timeout) {
            sessions[s].active = false;
            ESP_LOGI(TAG, "Session %d expired during cleanup", s);
        }
    }

    xSemaphoreGive(session_mutex);
}
