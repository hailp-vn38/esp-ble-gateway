#ifndef WEB_MODULES_H
#define WEB_MODULES_H

#include "esp_http_server.h"

esp_err_t web_assets_register_gateway(httpd_handle_t server);
esp_err_t web_assets_register_provisioning(httpd_handle_t server);

esp_err_t web_gateway_api_init(void);
esp_err_t web_gateway_api_register(httpd_handle_t server);

esp_err_t web_system_api_register_gateway(httpd_handle_t server);
esp_err_t web_system_api_register_provisioning(httpd_handle_t server);

esp_err_t web_wifi_api_init(void);
esp_err_t web_wifi_api_register(httpd_handle_t server);

esp_err_t web_ble_api_init(void);
esp_err_t web_ble_api_register(httpd_handle_t server);

#endif // WEB_MODULES_H
