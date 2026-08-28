#include "web_auth.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "web_auth_session";

#define SESSION_TOKEN_LEN 32
#define SESSION_TOKEN_BASE64_LEN 44

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

static void sha256_hash(const uint8_t *data, size_t len, uint8_t *hash)
{
    size_t out_len = 0;
    psa_hash_compute(PSA_ALG_SHA_256, data, len, hash, 32, &out_len);
}

esp_err_t web_auth_session_init(void)
{
    if (session_mutex != NULL) return ESP_ERR_INVALID_STATE;

    memset(sessions, 0, sizeof(sessions));
    session_mutex = xSemaphoreCreateMutex();
    return session_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t web_auth_session_create(char *token_out, size_t token_out_size)
{
    if (token_out == NULL || token_out_size < SESSION_TOKEN_BASE64_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t token[SESSION_TOKEN_LEN];
    esp_fill_random(token, sizeof(token));

    static const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    size_t i;
    for (i = 0; i < SESSION_TOKEN_LEN && i * 8 / 6 < token_out_size - 1; i++) {
        unsigned int bit_pos = i * 6;
        unsigned int byte_idx = bit_pos / 8;
        unsigned int bit_offset = bit_pos % 8;
        unsigned int val = (token[byte_idx] >> bit_offset) & 0x3F;
        if (bit_offset > 2 && byte_idx + 1 < sizeof(token)) {
            val |= (token[byte_idx + 1] << (8 - bit_offset)) & 0x3F;
        }
        token_out[i] = charset[val];
    }
    token_out[i] = '\0';

    uint8_t hash[32];
    sha256_hash(token, sizeof(token), hash);

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

    uint8_t decoded[SESSION_TOKEN_LEN];
    static const int8_t decode_table[256] = {
        ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,
        ['E'] = 4,  ['F'] = 5,  ['G'] = 6,  ['H'] = 7,
        ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11,
        ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15,
        ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19,
        ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
        ['Y'] = 24, ['Z'] = 25, ['a'] = 26, ['b'] = 27,
        ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31,
        ['g'] = 32, ['h'] = 33, ['i'] = 34, ['j'] = 35,
        ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39,
        ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43,
        ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47,
        ['w'] = 48, ['x'] = 49, ['y'] = 50, ['z'] = 51,
        ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
        ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59,
        ['8'] = 60, ['9'] = 61, ['-'] = 62, ['_'] = 63,
    };

    size_t token_len = strlen(token);
    if (token_len != 43) return WEB_AUTH_INVALID_CREDENTIALS;

    memset(decoded, 0, sizeof(decoded));
    size_t decoded_len = 0;
    for (size_t idx = 0; idx < token_len; idx++) {
        int8_t val = decode_table[(uint8_t)token[idx]];
        if (val < 0) return WEB_AUTH_INVALID_CREDENTIALS;

        unsigned int bit_pos = idx * 6;
        unsigned int byte_idx = bit_pos / 8;
        unsigned int bit_offset = bit_pos % 8;

        if (byte_idx < sizeof(decoded)) {
            decoded[byte_idx] |= (val << bit_offset) & 0xFF;
        }
        if (bit_offset > 2 && byte_idx + 1 < sizeof(decoded)) {
            decoded[byte_idx + 1] |= (val >> (8 - bit_offset)) & 0xFF;
        }
        decoded_len = byte_idx + 1;
    }

    if (decoded_len < sizeof(decoded)) {
        return WEB_AUTH_INVALID_CREDENTIALS;
    }

    uint8_t hash[32];
    sha256_hash(decoded, sizeof(decoded), hash);

    int64_t now = get_time_us();
    int64_t idle_timeout = (int64_t)CONFIG_WEB_AUTH_SESSION_IDLE_MINUTES * 60 * 1000000LL;
    int64_t max_timeout = (int64_t)CONFIG_WEB_AUTH_SESSION_MAX_HOURS * 3600 * 1000000LL;

    xSemaphoreTake(session_mutex, portMAX_DELAY);

    for (int s = 0; s < CONFIG_WEB_AUTH_MAX_SESSIONS; s++) {
        if (!sessions[s].active) continue;

        volatile uint8_t diff = 0;
        for (int j = 0; j < 32; j++) {
            diff |= sessions[s].token_hash[j] ^ hash[j];
        }

        if (diff == 0) {
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
    ESP_LOGI(TAG, "Session logout requested");
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
