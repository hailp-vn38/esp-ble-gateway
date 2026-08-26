#ifndef WEB_HTTP_H
#define WEB_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_http_server.h"

// Endpoint-specific body budgets (Plan v2 §34); buffers are stack-allocated
// by the handlers. A body of exactly capacity-1 bytes still fits the NUL.
#define WEB_DEVICE_BODY_MAX_LEN  512
#define WEB_COMMAND_BODY_MAX_LEN 1024
#define WEB_WIFI_BODY_MAX_LEN    256

// Absolute receive deadline covering the whole body (Plan v2 §28): socket
// timeout retries stay bounded regardless of client behaviour.
#define WEB_BODY_RECEIVE_TIMEOUT_MS 3000

#define WEB_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

typedef enum {
    WEB_BODY_OK = 0,
    WEB_BODY_EMPTY,
    WEB_BODY_TOO_LARGE,
    WEB_BODY_TIMEOUT,
    WEB_BODY_IO_ERROR,
    WEB_BODY_INVALID_JSON,
} web_body_status_t;

esp_err_t web_send_json(httpd_req_t *request, cJSON *json);
esp_err_t web_send_api_error(httpd_req_t *request, const char *status,
                             const char *message);
// Maps a typed body failure onto its HTTP status (Plan v2 §31) and closes
// the connection whenever unread body bytes would desync keep-alive (§32).
esp_err_t web_send_body_error(httpd_req_t *request, web_body_status_t status);
// Parses a JSON object body into the caller's buffer. On failure returns
// NULL and sets *status so the caller can answer via web_send_body_error().
cJSON *web_parse_request_json(httpd_req_t *request, char *buffer,
                              size_t capacity, web_body_status_t *status);
const char *web_get_json_string(const cJSON *object, const char *key,
                                size_t max_length, bool required);
esp_err_t web_register_routes(httpd_handle_t server, const httpd_uri_t *routes,
                              size_t route_count);

#endif // WEB_HTTP_H
