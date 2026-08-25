#include "web_http.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "web_http";

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

static int read_request_body(httpd_req_t *request, char *buffer, size_t capacity)
{
    if (request->content_len <= 0 || request->content_len >= (int)capacity) {
        return -1;
    }

    int total = 0;
    while (total < request->content_len) {
        int received = httpd_req_recv(request, buffer + total,
                                      request->content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0) return -1;
        total += received;
    }
    buffer[total] = '\0';
    return total;
}

cJSON *web_parse_request_json(httpd_req_t *request, char *buffer,
                              size_t capacity)
{
    if (read_request_body(request, buffer, capacity) < 0) return NULL;

    cJSON *json = cJSON_Parse(buffer);
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return NULL;
    }
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
