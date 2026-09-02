#ifndef WEB_MODULES_H
#define WEB_MODULES_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"
#include "command_dispatcher.h"

/* Wrapper around httpd_uri_t that default-initializes WS fields to false.
 * Required when CONFIG_HTTPD_WS_SUPPORT=y to avoid missing-field-initializer
 * warnings in existing non-WebSocket route definitions. */
#define WEB_URI_INIT(uri_val, method_val, handler_val) \
    { .uri = (uri_val), .method = (method_val), .handler = (handler_val), \
      .user_ctx = NULL, .is_websocket = false, \
      .handle_ws_control_frames = false, .supported_subprotocol = NULL }

esp_err_t web_assets_register_gateway(httpd_handle_t server);
esp_err_t web_assets_register_provisioning(httpd_handle_t server);
esp_err_t web_assets_register_provisioning_errors(httpd_handle_t server);

esp_err_t web_gateway_api_init(void);
esp_err_t web_gateway_api_register(httpd_handle_t server);

esp_err_t web_device_api_register(httpd_handle_t server);
esp_err_t web_command_api_register(httpd_handle_t server);
esp_err_t web_device_schema_api_register(httpd_handle_t server);

esp_err_t web_system_api_register_gateway(httpd_handle_t server);
esp_err_t web_system_api_register_provisioning(httpd_handle_t server);

esp_err_t web_wifi_api_init(void);
esp_err_t web_wifi_api_register(httpd_handle_t server);

esp_err_t web_ble_api_init(void);
esp_err_t web_ble_api_register(httpd_handle_t server);

esp_err_t web_exposure_api_register(httpd_handle_t server);

esp_err_t web_mcp_token_api_register(httpd_handle_t server);
esp_err_t web_mcp_token_get_status(bool *configured, char *preview,
                                   size_t preview_size);

esp_err_t web_settings_api_register(httpd_handle_t server);

esp_err_t web_event_ws_init(void);
esp_err_t web_event_ws_register(httpd_handle_t server);

// Shared helper used by command and device APIs for async dispatch results.
void web_send_dispatch_result(httpd_req_t *request,
                              const dispatch_result_t *result);

#endif // WEB_MODULES_H
