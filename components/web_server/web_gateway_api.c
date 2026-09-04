#include "web_modules.h"
#include "esp_log.h"

static const char *TAG = "web_gateway_api";

esp_err_t web_gateway_api_init(void)
{
    return ESP_OK;
}

esp_err_t web_gateway_api_register(httpd_handle_t server)
{
    esp_err_t err;
    err = web_device_api_register(server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device API: %s", esp_err_to_name(err));
        return err;
    }
    err = web_command_api_register(server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "command API: %s", esp_err_to_name(err));
        return err;
    }
    err = web_device_schema_api_register(server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device schema API: %s", esp_err_to_name(err));
        return err;
    }
    err = web_device_detail_api_register(server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device detail API: %s", esp_err_to_name(err));
        return err;
    }
    err = web_exposure_api_register(server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "exposure API: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
