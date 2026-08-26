#include "web_http.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "web_http";

// Common hardening headers on every API response (Plan v2 §64, P2 review).
static void set_security_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
}

esp_err_t web_send_json(httpd_req_t *request, cJSON *json)
{
    if (json == NULL) {
        httpd_resp_send_500(request);
        return ESP_ERR_NO_MEM;
    }

    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (text == NULL) {
        httpd_resp_send_500(request);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    set_security_headers(request);
    esp_err_t result = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return result;
}

esp_err_t web_send_api_error(httpd_req_t *request, const char *status,
                             const char *message)
{
    httpd_resp_set_status(request, status);
    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddBoolToObject(json, "success", false);
        cJSON_AddStringToObject(json, "message", message);
    }
    return web_send_json(request, json);
}

esp_err_t web_send_body_error(httpd_req_t *request, web_body_status_t status)
{
    const char *http_status = "500 Internal Server Error";
    const char *message = "Could not process the request body";
    const char *code = "internal_error";
    bool close_conn = true;

    switch (status) {
    case WEB_BODY_OK:
        return ESP_OK;
    case WEB_BODY_EMPTY:
        http_status = "400 Bad Request";
        message = "Empty request body";
        code = "invalid_request";
        close_conn = false; // nothing was expected on the socket
        break;
    case WEB_BODY_INVALID_JSON:
        http_status = "400 Bad Request";
        message = "Invalid JSON body";
        code = "invalid_request";
        close_conn = false; // body was fully consumed
        break;
    case WEB_BODY_TOO_LARGE:
        http_status = "413 Content Too Large";
        message = "Request body exceeds the endpoint limit";
        code = "payload_too_large";
        break;
    case WEB_BODY_TIMEOUT:
        http_status = "408 Request Timeout";
        message = "Request body did not arrive in time";
        code = "request_timeout";
        break;
    case WEB_BODY_IO_ERROR:
        http_status = "400 Bad Request";
        message = "Could not read the request body";
        code = "internal_error";
        break;
    }

    // Unread or partially-read bodies would be parsed as the next pipelined
    // request (Plan v2 §32): keep-alive is only safe when the body finished.
    if (close_conn) {
        httpd_resp_set_hdr(request, "Connection", "close");
    }
    httpd_resp_set_status(request, http_status);
    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddBoolToObject(json, "success", false);
        cJSON_AddStringToObject(json, "message", message);
        cJSON *error = cJSON_AddObjectToObject(json, "error");
        if (error != NULL) cJSON_AddStringToObject(error, "code", code);
    }
    return web_send_json(request, json);
}

static web_body_status_t read_request_body(httpd_req_t *request, char *buffer,
                                           size_t capacity)
{
    int content_len = request->content_len;
    if (content_len <= 0) return WEB_BODY_EMPTY;
    if (content_len >= (int)capacity) return WEB_BODY_TOO_LARGE;

    int64_t deadline =
        esp_timer_get_time() + WEB_BODY_RECEIVE_TIMEOUT_MS * 1000LL;
    int total = 0;
    while (total < content_len) {
        if (esp_timer_get_time() >= deadline) return WEB_BODY_TIMEOUT;
        int received = httpd_req_recv(request, buffer + total,
                                      content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0) return WEB_BODY_IO_ERROR;
        total += received;
    }
    buffer[total] = '\0';
    return WEB_BODY_OK;
}

cJSON *web_parse_request_json(httpd_req_t *request, char *buffer,
                              size_t capacity, web_body_status_t *status)
{
    web_body_status_t read_status =
        read_request_body(request, buffer, capacity);
    if (read_status != WEB_BODY_OK) {
        *status = read_status;
        return NULL;
    }

    cJSON *json = cJSON_Parse(buffer);
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        *status = WEB_BODY_INVALID_JSON;
        return NULL;
    }
    *status = WEB_BODY_OK;
    return json;
}

const char *web_get_json_string(const cJSON *object, const char *key,
                                size_t max_length, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (item == NULL && !required) return NULL;
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        (required && item->valuestring[0] == '\0') ||
        strnlen(item->valuestring, max_length) >= max_length) {
        return NULL;
    }
    return item->valuestring;
}

esp_err_t web_register_routes(httpd_handle_t server, const httpd_uri_t *routes,
                              size_t route_count)
{
    for (size_t i = 0; i < route_count; i++) {
        esp_err_t error = httpd_register_uri_handler(server, &routes[i]);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Could not register %s: %s", routes[i].uri,
                     esp_err_to_name(error));
            return error;
        }
    }
    return ESP_OK;
}
