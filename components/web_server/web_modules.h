#ifndef WEB_MODULES_H
#define WEB_MODULES_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

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
size_t web_command_active_contexts(void);
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
void web_event_ws_get_stats(int *active_clients, uint32_t *ring_used,
                            bool *resync_pending, uint32_t *resync_total,
                            uint32_t *send_error_total,
                            uint32_t *connect_total,
                            uint32_t *disconnect_total);

#endif // WEB_MODULES_H
