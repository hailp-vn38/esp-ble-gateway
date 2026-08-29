#include "web_auth.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "web_auth_store.h"
#include "web_auth_password.h"
#include "web_auth_session.h"

static const char *TAG = "web_auth";

// Rate limiter state
typedef struct {
    uint32_t failure_count;
    int64_t window_start_us;
} rate_limiter_t;

static rate_limiter_t rate_limiter = {0};
#define RATE_LIMIT_THRESHOLD 5
#define RATE_LIMIT_WINDOW_US (60 * 1000000LL)  // 60 seconds

static bool rate_limit_check(void)
{
    int64_t now = esp_timer_get_time();
    if (now - rate_limiter.window_start_us > RATE_LIMIT_WINDOW_US) {
        rate_limiter.failure_count = 0;
        rate_limiter.window_start_us = now;
    }
    return rate_limiter.failure_count < RATE_LIMIT_THRESHOLD;
}

static void rate_limit_record_failure(void)
{
    rate_limiter.failure_count++;
}

static void rate_limit_reset(void)
{
    rate_limiter.failure_count = 0;
}

// Runtime auth state — NVS is only touched at boot/login/config mutation.
static struct {
    bool loaded;
    bool enabled;
    bool configured;
    char username[33];
} auth_state;
static SemaphoreHandle_t auth_state_mutex;

static void auth_state_update(bool enabled, bool configured,
                              const char *username)
{
    xSemaphoreTake(auth_state_mutex, portMAX_DELAY);
    auth_state.loaded = true;
    auth_state.enabled = enabled;
    auth_state.configured = configured;
    strlcpy(auth_state.username, username != NULL ? username : "",
            sizeof(auth_state.username));
    xSemaphoreGive(auth_state_mutex);
}

static esp_err_t auth_state_load(void)
{
    bool enabled = false;
    char username[33] = {0};
    uint8_t pwd_salt[16];
    uint8_t pwd_hash[32];
    uint32_t pwd_iter;

    esp_err_t err = web_auth_store_load(&enabled, username, sizeof(username),
                                        pwd_salt, pwd_hash, &pwd_iter);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        auth_state_update(false, false, "");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not load auth state: %s", esp_err_to_name(err));
        return err;
    }
    auth_state_update(enabled, true, username);
    return ESP_OK;
}

int web_auth_get_pbkdf2_iterations(void)
{
    return CONFIG_WEB_AUTH_PBKDF2_ITERATIONS;
}

esp_err_t web_auth_init(void)
{
    ESP_LOGI(TAG, "Initializing web authentication");

    if (auth_state_mutex == NULL) {
        auth_state_mutex = xSemaphoreCreateMutex();
        if (auth_state_mutex == NULL) return ESP_ERR_NO_MEM;
    }

    esp_err_t err = web_auth_session_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize session manager");
        return err;
    }

    err = auth_state_load();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Web auth initialized (max sessions: %d, idle timeout: %d min, max hours: %d)",
             CONFIG_WEB_AUTH_MAX_SESSIONS,
             CONFIG_WEB_AUTH_SESSION_IDLE_MINUTES,
             CONFIG_WEB_AUTH_SESSION_MAX_HOURS);

    return ESP_OK;
}

esp_err_t web_auth_get_status(web_auth_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    if (auth_state_mutex == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(auth_state_mutex, portMAX_DELAY);
    bool loaded = auth_state.loaded;
    xSemaphoreGive(auth_state_mutex);
    if (!loaded) {
        esp_err_t err = auth_state_load();
        if (err != ESP_OK) return err;
    }

    xSemaphoreTake(auth_state_mutex, portMAX_DELAY);
    out->enabled = auth_state.enabled;
    out->credentials_configured = auth_state.configured;
    strlcpy(out->username, auth_state.username, sizeof(out->username));
    xSemaphoreGive(auth_state_mutex);

    return ESP_OK;
}

web_auth_result_t web_auth_login(const char *username,
                                 const char *password,
                                 char *session_token,
                                 size_t session_token_size)
{
    if (username == NULL || password == NULL) {
        return WEB_AUTH_INVALID_CREDENTIALS;
    }

    // Check rate limit
    if (!rate_limit_check()) {
        ESP_LOGW(TAG, "Login rate limited");
        return WEB_AUTH_RATE_LIMITED;
    }

    // Load stored credentials
    bool enabled = false;
    char stored_username[33] = {0};
    uint8_t pwd_salt[16];
    uint8_t pwd_hash[32];
    uint32_t pwd_iter;

    esp_err_t err = web_auth_store_load(&enabled, stored_username, sizeof(stored_username),
                                        pwd_salt, pwd_hash, &pwd_iter);
    if (err != ESP_OK) {
        return WEB_AUTH_NOT_CONFIGURED;
    }

    if (!enabled) {
        return WEB_AUTH_NOT_CONFIGURED;
    }

    // Verify username
    if (strcmp(username, stored_username) != 0) {
        rate_limit_record_failure();
        return WEB_AUTH_INVALID_CREDENTIALS;
    }

    // Verify password
    err = web_auth_password_verify(password, pwd_salt, sizeof(pwd_salt),
                                   pwd_hash, sizeof(pwd_hash), pwd_iter);
    if (err != ESP_OK) {
        rate_limit_record_failure();
        return WEB_AUTH_INVALID_CREDENTIALS;
    }

    // Create session
    err = web_auth_session_create(session_token, session_token_size);
    if (err != ESP_OK) {
        return WEB_AUTH_STORAGE_ERROR;
    }

    rate_limit_reset();
    ESP_LOGI(TAG, "Login successful for user: %s", username);
    return WEB_AUTH_OK;
}

web_auth_result_t web_auth_validate_session(const char *session_token)
{
    if (session_token == NULL || session_token[0] == '\0') {
        return WEB_AUTH_REQUIRED;
    }

    web_auth_status_t status;
    if (web_auth_get_status(&status) != ESP_OK) {
        return WEB_AUTH_STORAGE_ERROR;
    }
    if (!status.enabled) return WEB_AUTH_OK;

    return web_auth_session_validate(session_token);
}

void web_auth_logout(const char *session_token)
{
    web_auth_session_destroy(session_token);
}

void web_auth_invalidate_all_sessions(void)
{
    web_auth_session_destroy_all();
    ESP_LOGI(TAG, "All sessions invalidated");
}

web_auth_result_t web_auth_enable(const char *username,
                                  const char *current_password,
                                  const char *new_password)
{
    bool has_creds = web_auth_store_has_credentials();

    if (new_password != NULL) {
        // Full enable: validate and persist credentials
        if (username == NULL) {
            return WEB_AUTH_INVALID_CREDENTIALS;
        }

        if (!web_auth_username_validate(username)) {
            return WEB_AUTH_INVALID_USERNAME;
        }

        if (!web_auth_password_validate(new_password)) {
            return WEB_AUTH_INVALID_PASSWORD;
        }

        if (has_creds) {
            if (current_password == NULL || current_password[0] == '\0') {
                return WEB_AUTH_CURRENT_PASSWORD_REQUIRED;
            }

            bool enabled = false;
            char stored_username[33] = {0};
            uint8_t pwd_salt[16];
            uint8_t pwd_hash[32];
            uint32_t pwd_iter;

            esp_err_t err = web_auth_store_load(&enabled, stored_username, sizeof(stored_username),
                                                pwd_salt, pwd_hash, &pwd_iter);
            if (err == ESP_OK && stored_username[0] != '\0') {
                err = web_auth_password_verify(current_password, pwd_salt, sizeof(pwd_salt),
                                               pwd_hash, sizeof(pwd_hash), pwd_iter);
                if (err != ESP_OK) {
                    return WEB_AUTH_CURRENT_PASSWORD_INVALID;
                }
            }
        }

        uint8_t new_salt[16];
        uint8_t new_hash[32];
        uint32_t new_iter;

        esp_err_t err = web_auth_password_hash(new_password, new_salt, sizeof(new_salt),
                                               new_hash, sizeof(new_hash), &new_iter);
        if (err != ESP_OK) {
            return WEB_AUTH_STORAGE_ERROR;
        }

        err = web_auth_store_save(true, username, new_salt, new_hash, new_iter);
        if (err != ESP_OK) {
            return WEB_AUTH_STORAGE_ERROR;
        }
    } else {
        // Re-enable only: credentials already exist, just flip enabled flag
        if (!has_creds) {
            return WEB_AUTH_NOT_CONFIGURED;
        }

        if (current_password == NULL || current_password[0] == '\0') {
            return WEB_AUTH_CURRENT_PASSWORD_REQUIRED;
        }

        bool enabled = false;
        char stored_username[33] = {0};
        uint8_t pwd_salt[16];
        uint8_t pwd_hash[32];
        uint32_t pwd_iter;

        esp_err_t err = web_auth_store_load(&enabled, stored_username, sizeof(stored_username),
                                            pwd_salt, pwd_hash, &pwd_iter);
        if (err != ESP_OK) {
            return WEB_AUTH_NOT_CONFIGURED;
        }

        err = web_auth_password_verify(current_password, pwd_salt, sizeof(pwd_salt),
                                       pwd_hash, sizeof(pwd_hash), pwd_iter);
        if (err != ESP_OK) {
            return WEB_AUTH_CURRENT_PASSWORD_INVALID;
        }

        err = web_auth_store_set_enabled(true);
        if (err != ESP_OK) {
            return WEB_AUTH_STORAGE_ERROR;
        }
    }

    web_auth_invalidate_all_sessions();
    auth_state_update(true, true, username);

    ESP_LOGI(TAG, "Authentication enabled");
    return WEB_AUTH_OK;
}

web_auth_result_t web_auth_disable(const char *current_password)
{
    if (current_password == NULL || current_password[0] == '\0') {
        return WEB_AUTH_CURRENT_PASSWORD_REQUIRED;
    }

    // Verify current password
    bool enabled = false;
    char username[33] = {0};
    uint8_t pwd_salt[16];
    uint8_t pwd_hash[32];
    uint32_t pwd_iter;

    esp_err_t err = web_auth_store_load(&enabled, username, sizeof(username),
                                        pwd_salt, pwd_hash, &pwd_iter);
    if (err != ESP_OK) {
        return WEB_AUTH_NOT_CONFIGURED;
    }

    err = web_auth_password_verify(current_password, pwd_salt, sizeof(pwd_salt),
                                   pwd_hash, sizeof(pwd_hash), pwd_iter);
    if (err != ESP_OK) {
        return WEB_AUTH_CURRENT_PASSWORD_INVALID;
    }

    // Disable auth (keep credentials)
    err = web_auth_store_set_enabled(false);
    if (err != ESP_OK) {
        return WEB_AUTH_STORAGE_ERROR;
    }

    // Invalidate all sessions
    web_auth_invalidate_all_sessions();
    auth_state_update(false, true, username);

    ESP_LOGI(TAG, "Authentication disabled");
    return WEB_AUTH_OK;
}

web_auth_result_t web_auth_change_username(const char *current_password,
                                           const char *new_username)
{
    if (current_password == NULL || current_password[0] == '\0') {
        return WEB_AUTH_CURRENT_PASSWORD_REQUIRED;
    }

    if (new_username == NULL || !web_auth_username_validate(new_username)) {
        return WEB_AUTH_INVALID_USERNAME;
    }

    // Verify current password
    bool enabled = false;
    char username[33] = {0};
    uint8_t pwd_salt[16];
    uint8_t pwd_hash[32];
    uint32_t pwd_iter;

    esp_err_t err = web_auth_store_load(&enabled, username, sizeof(username),
                                        pwd_salt, pwd_hash, &pwd_iter);
    if (err != ESP_OK) {
        return WEB_AUTH_NOT_CONFIGURED;
    }

    err = web_auth_password_verify(current_password, pwd_salt, sizeof(pwd_salt),
                                   pwd_hash, sizeof(pwd_hash), pwd_iter);
    if (err != ESP_OK) {
        return WEB_AUTH_CURRENT_PASSWORD_INVALID;
    }

    // Save with new username
    err = web_auth_store_save(enabled, new_username, pwd_salt, pwd_hash, pwd_iter);
    if (err != ESP_OK) {
        return WEB_AUTH_STORAGE_ERROR;
    }

    // Invalidate all sessions
    web_auth_invalidate_all_sessions();
    auth_state_update(enabled, true, new_username);

    ESP_LOGI(TAG, "Username changed to: %s", new_username);
    return WEB_AUTH_OK;
}

web_auth_result_t web_auth_change_password(const char *current_password,
                                           const char *new_password)
{
    if (current_password == NULL || current_password[0] == '\0') {
        return WEB_AUTH_CURRENT_PASSWORD_REQUIRED;
    }

    if (new_password == NULL || !web_auth_password_validate(new_password)) {
        return WEB_AUTH_INVALID_PASSWORD;
    }

    // Verify current password
    bool enabled = false;
    char username[33] = {0};
    uint8_t pwd_salt[16];
    uint8_t pwd_hash[32];
    uint32_t pwd_iter;

    esp_err_t err = web_auth_store_load(&enabled, username, sizeof(username),
                                        pwd_salt, pwd_hash, &pwd_iter);
    if (err != ESP_OK) {
        return WEB_AUTH_NOT_CONFIGURED;
    }

    err = web_auth_password_verify(current_password, pwd_salt, sizeof(pwd_salt),
                                   pwd_hash, sizeof(pwd_hash), pwd_iter);
    if (err != ESP_OK) {
        return WEB_AUTH_CURRENT_PASSWORD_INVALID;
    }

    // Generate new password hash
    uint8_t new_salt[16];
    uint8_t new_hash[32];
    uint32_t new_iter;

    err = web_auth_password_hash(new_password, new_salt, sizeof(new_salt),
                                 new_hash, sizeof(new_hash), &new_iter);
    if (err != ESP_OK) {
        return WEB_AUTH_STORAGE_ERROR;
    }

    // Save new password
    err = web_auth_store_save(enabled, username, new_salt, new_hash, new_iter);
    if (err != ESP_OK) {
        return WEB_AUTH_STORAGE_ERROR;
    }

    // Invalidate all sessions
    web_auth_invalidate_all_sessions();
    auth_state_update(enabled, true, username);

    ESP_LOGI(TAG, "Password changed for user: %s", username);
    return WEB_AUTH_OK;
}

int web_auth_session_cookie_max_age_seconds(void)
{
    return CONFIG_WEB_AUTH_SESSION_MAX_HOURS * 60 * 60;
}
