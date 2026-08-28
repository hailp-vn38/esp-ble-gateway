#include "web_auth.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *NVS_NAMESPACE = "web_auth";

typedef struct {
    uint8_t ver;
    uint8_t enabled;
    char username[33];
    uint8_t pwd_salt[16];
    uint8_t pwd_hash[32];
    uint32_t pwd_iter;
} web_auth_nvs_data_t;

static esp_err_t nvs_read_data(web_auth_nvs_data_t *data)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = sizeof(data->ver);
    err = nvs_get_blob(handle, "ver", &data->ver, &len);
    if (err != ESP_OK) goto cleanup;

    len = sizeof(data->enabled);
    nvs_get_blob(handle, "enabled", &data->enabled, &len);

    len = sizeof(data->username);
    nvs_get_blob(handle, "username", data->username, &len);

    len = sizeof(data->pwd_salt);
    nvs_get_blob(handle, "pwd_salt", data->pwd_salt, &len);

    len = sizeof(data->pwd_hash);
    nvs_get_blob(handle, "pwd_hash", data->pwd_hash, &len);

    len = sizeof(data->pwd_iter);
    nvs_get_blob(handle, "pwd_iter", &data->pwd_iter, &len);

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
    web_auth_nvs_data_t data = {0};
    data.ver = 1;
    data.enabled = enabled ? 1 : 0;
    strlcpy(data.username, username, sizeof(data.username));
    memcpy(data.pwd_salt, pwd_salt, sizeof(data.pwd_salt));
    memcpy(data.pwd_hash, pwd_hash, sizeof(data.pwd_hash));
    data.pwd_iter = pwd_iter;

    return nvs_write_data(&data);
}

esp_err_t web_auth_store_set_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    uint8_t val = enabled ? 1 : 0;
    err = nvs_set_blob(handle, "enabled", &val, sizeof(val));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

bool web_auth_store_has_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;

    size_t len = 33;
    char username[33] = {0};
    err = nvs_get_blob(handle, "username", username, &len);
    nvs_close(handle);

    return err == ESP_OK && username[0] != '\0';
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
