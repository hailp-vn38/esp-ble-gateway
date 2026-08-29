#include "web_auth.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *NVS_NAMESPACE = "web_auth";

#define WEB_AUTH_STORE_VERSION 1
#define WEB_AUTH_MIN_ITERATIONS 10000U
#define WEB_AUTH_MAX_ITERATIONS 200000U

typedef struct {
    uint8_t ver;
    uint8_t enabled;
    char username[33];
    uint8_t pwd_salt[16];
    uint8_t pwd_hash[32];
    uint32_t pwd_iter;
} web_auth_nvs_data_t;

static esp_err_t read_blob_exact(nvs_handle_t handle, const char *key,
                                 void *output, size_t expected_size)
{
    size_t actual_size = expected_size;
    esp_err_t err = nvs_get_blob(handle, key, output, &actual_size);
    if (err != ESP_OK) return err;
    return actual_size == expected_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t validate_data(const web_auth_nvs_data_t *data)
{
    if (data->ver != WEB_AUTH_STORE_VERSION || data->enabled > 1 ||
        data->username[sizeof(data->username) - 1] != '\0' ||
        memchr(data->username, '\0', sizeof(data->username)) == NULL ||
        !web_auth_username_validate(data->username) ||
        data->pwd_iter < WEB_AUTH_MIN_ITERATIONS ||
        data->pwd_iter > WEB_AUTH_MAX_ITERATIONS) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t nvs_read_data(web_auth_nvs_data_t *data)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = read_blob_exact(handle, "ver", &data->ver, sizeof(data->ver));
    if (err != ESP_OK) goto cleanup;

    err = read_blob_exact(handle, "enabled", &data->enabled,
                          sizeof(data->enabled));
    if (err != ESP_OK) goto cleanup;

    err = read_blob_exact(handle, "username", data->username,
                          sizeof(data->username));
    if (err != ESP_OK) goto cleanup;

    err = read_blob_exact(handle, "pwd_salt", data->pwd_salt,
                          sizeof(data->pwd_salt));
    if (err != ESP_OK) goto cleanup;

    err = read_blob_exact(handle, "pwd_hash", data->pwd_hash,
                          sizeof(data->pwd_hash));
    if (err != ESP_OK) goto cleanup;

    err = read_blob_exact(handle, "pwd_iter", &data->pwd_iter,
                          sizeof(data->pwd_iter));
    if (err != ESP_OK) goto cleanup;

    err = validate_data(data);

cleanup:
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_write_data(const web_auth_nvs_data_t *data)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, "ver", &data->ver, sizeof(data->ver));
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_blob(handle, "enabled", &data->enabled, sizeof(data->enabled));
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_blob(handle, "username", data->username, sizeof(data->username));
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_blob(handle, "pwd_salt", data->pwd_salt, sizeof(data->pwd_salt));
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_blob(handle, "pwd_hash", data->pwd_hash, sizeof(data->pwd_hash));
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_blob(handle, "pwd_iter", &data->pwd_iter, sizeof(data->pwd_iter));
    if (err != ESP_OK) goto cleanup;

    err = nvs_commit(handle);

cleanup:
    nvs_close(handle);
    return err;
}

esp_err_t web_auth_store_load(bool *enabled, char *username, size_t username_size,
                              uint8_t *pwd_salt, uint8_t *pwd_hash, uint32_t *pwd_iter)
{
    if (enabled == NULL || username == NULL || username_size == 0 ||
        pwd_salt == NULL || pwd_hash == NULL || pwd_iter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    web_auth_nvs_data_t data = {0};
    esp_err_t err = nvs_read_data(&data);
    if (err != ESP_OK) return err;

    *enabled = data.enabled != 0;
    strlcpy(username, data.username, username_size);
    memcpy(pwd_salt, data.pwd_salt, sizeof(data.pwd_salt));
    memcpy(pwd_hash, data.pwd_hash, sizeof(data.pwd_hash));
    *pwd_iter = data.pwd_iter;

    return ESP_OK;
}

esp_err_t web_auth_store_save(bool enabled, const char *username,
                              const uint8_t *pwd_salt, const uint8_t *pwd_hash,
                              uint32_t pwd_iter)
{
    if (username == NULL || pwd_salt == NULL || pwd_hash == NULL ||
        !web_auth_username_validate(username) ||
        pwd_iter < WEB_AUTH_MIN_ITERATIONS ||
        pwd_iter > WEB_AUTH_MAX_ITERATIONS) {
        return ESP_ERR_INVALID_ARG;
    }

    web_auth_nvs_data_t data = {0};
    data.ver = WEB_AUTH_STORE_VERSION;
    data.enabled = enabled ? 1 : 0;
    strlcpy(data.username, username, sizeof(data.username));
    memcpy(data.pwd_salt, pwd_salt, sizeof(data.pwd_salt));
    memcpy(data.pwd_hash, pwd_hash, sizeof(data.pwd_hash));
    data.pwd_iter = pwd_iter;

    return nvs_write_data(&data);
}

esp_err_t web_auth_store_set_enabled(bool enabled)
{
    web_auth_nvs_data_t data = {0};
    esp_err_t err = nvs_read_data(&data);
    if (err != ESP_OK) return err;
    data.enabled = enabled ? 1 : 0;
    return nvs_write_data(&data);
}

bool web_auth_store_has_credentials(void)
{
    bool enabled = false;
    char username[33] = {0};
    uint8_t salt[16];
    uint8_t hash[32];
    uint32_t iterations = 0;
    return web_auth_store_load(&enabled, username, sizeof(username), salt,
                               hash, &iterations) == ESP_OK;
}

esp_err_t web_auth_store_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
