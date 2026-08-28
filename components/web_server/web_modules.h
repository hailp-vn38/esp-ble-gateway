#ifndef WEB_MODULES_H
#define WEB_MODULES_H

#include "esp_http_server.h"
#include "command_dispatcher.h"

esp_err_t web_assets_register_gateway(httpd_handle_t server);
esp_err_t web_assets_register_provisioning(httpd_handle_t server);
esp_err_t web_assets_register_provisioning_errors(httpd_handle_t server);

esp_err_t web_gateway_api_init(void);
esp_err_t web_gateway_api_register(httpd_handle_t server);

esp_err_t web_device_api_register(httpd_handle_t server);
esp_err_t web_command_api_register(httpd_handle_t server);
esp_err_t web_capability_api_register(httpd_handle_t server);

esp_err_t web_system_api_register_gateway(httpd_handle_t server);
esp_err_t web_system_api_register_provisioning(httpd_handle_t server);

esp_err_t web_wifi_api_init(void);
esp_err_t web_wifi_api_register(httpd_handle_t server);

esp_err_t web_ble_api_init(void);
esp_err_t web_ble_api_register(httpd_handle_t server);

esp_err_t web_exposure_api_register(httpd_handle_t server);

esp_err_t web_auth_api_register(httpd_handle_t server);

esp_err_t web_mcp_token_api_register(httpd_handle_t server);

// Shared helper used by command and device APIs for async dispatch results.
void web_send_dispatch_result(httpd_req_t *request,
                              const dispatch_result_t *result);

#endif // WEB_MODULES_H
