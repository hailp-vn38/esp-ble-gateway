#ifndef WEB_HTTP_H
#define WEB_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_http_server.h"

#define WEB_REQUEST_BODY_MAX_LEN 1024
#define WEB_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

esp_err_t web_send_json(httpd_req_t *request, cJSON *json);
esp_err_t web_send_api_error(httpd_req_t *request, const char *status,
                             const char *message);
cJSON *web_parse_request_json(httpd_req_t *request, char *buffer,
                              size_t capacity);
const char *web_get_json_string(const cJSON *object, const char *key,
                                size_t max_length, bool required);
esp_err_t web_register_routes(httpd_handle_t server, const httpd_uri_t *routes,
                              size_t route_count);

#endif // WEB_HTTP_H
