#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "command_dispatcher.h"
#include "command_executor.h"
#include "web_http.h"

static const char *TAG = "web_device_api";

typedef struct {
    httpd_req_t *request;
} command_async_context_t;

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int parse_ble_addr(const char *text, uint8_t address[6])
{
    if (text == NULL) return -1;

    uint8_t display[6];
    for (int i = 0; i < 6; i++) {
        int high = hex_value(*text++);
        int low = high >= 0 ? hex_value(*text++) : -1;
        if (high < 0 || low < 0) return -1;
        display[i] = (uint8_t)((high << 4) | low);
        if (i < 5) {
            if (*text != ':' && *text != '-') return -1;
            text++;
        }
    }
    if (*text != '\0') return -1;

    for (int i = 0; i < 6; i++) address[i] = display[5 - i];
    return 0;
}

static esp_err_t dispatch_message_async(httpd_req_t *request,
                                        const gw_message_t *message);

static int fill_device_message(const cJSON *json, gw_message_t *message,
                               const char *command, bool require_metadata)
{
    memset(message, 0, sizeof(*message));
    message->protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(message->type, "gateway_command", sizeof(message->type));
    strlcpy(message->command, command, sizeof(message->command));

    const char *device_id = web_get_json_string(
        json, "device_id", sizeof(message->device_id), true);
    if (device_id == NULL) return -1;
    strlcpy(message->device_id, device_id, sizeof(message->device_id));
    message->has_device_id = true;

    if (cJSON_GetObjectItemCaseSensitive(json, "name") != NULL) {
        const char *name = web_get_json_string(
            json, "name", sizeof(message->name), true);
        if (name == NULL) return -1;
        strlcpy(message->name, name, sizeof(message->name));
    }
    // "type" is no longer part of the device API: protocol v4 messages
    // carry no device-level type (plan §5/§19).
    if (require_metadata && message->name[0] == '\0') {
        strlcpy(message->name, message->device_id, sizeof(message->name));
    }

    if (cJSON_GetObjectItemCaseSensitive(json, "ble_addr") != NULL) {
        const char *address = web_get_json_string(json, "ble_addr", 18, true);
        if (address == NULL || parse_ble_addr(address, message->ble_addr) != 0) {
            return -1;
        }

        const cJSON *address_type =
            cJSON_GetObjectItemCaseSensitive(json, "ble_addr_type");
        if (address_type != NULL &&
            (!cJSON_IsNumber(address_type) || address_type->valueint < 0 ||
             address_type->valueint > UINT8_MAX ||
             address_type->valuedouble != (double)address_type->valueint)) {
            return -1;
        }
        message->ble_addr_type =
            address_type != NULL ? (uint8_t)address_type->valueint : 0;
        message->has_ble_addr = true;
    }
    return 0;
}

static esp_err_t devices_get_handler(httpd_req_t *request)
{
    gw_message_t message = {.protocol_version = GW_PROTOCOL_VERSION};
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, "list_devices", sizeof(message.command));

    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    if (!dispatch_result_is_ok(&result) ||
        result.format != DISPATCH_RESULT_JSON) {
        return web_send_api_error(request, "500 Internal Server Error",
                                  result.payload);
    }

    cJSON *array = cJSON_Parse(result.payload);
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(array);
        return web_send_api_error(request, "500 Internal Server Error",
                                  "Dispatcher returned an invalid device list");
    }
    return web_send_json(request, array);
}

static esp_err_t devices_write_handler(httpd_req_t *request)
{
    char body[WEB_DEVICE_BODY_MAX_LEN];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) {
        return web_send_body_error(request, body_status);
    }

    const char *command =
        request->method == HTTP_POST ? "add_device" : "edit_device";
    gw_message_t message;
    int parse_result = fill_device_message(json, &message, command,
                                           request->method == HTTP_POST);
    cJSON_Delete(json);
    if (parse_result != 0) {
        return web_send_api_error_code(request, "400 Bad Request",
                                        "Invalid device fields",
                                        "invalid_request");
    }

    return dispatch_message_async(request, &message);
}

static esp_err_t devices_delete_handler(httpd_req_t *request)
{
    char query[128];
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        web_get_query_value(query, "device_id", device_id,
                            sizeof(device_id)) != ESP_OK ||
        device_id[0] == '\0') {
        return web_send_api_error_code(request, "400 Bad Request", "Missing device_id",
                                        "invalid_request");
    }

    gw_message_t message = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .has_device_id = true,
    };
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, "delete_device", sizeof(message.command));
    strlcpy(message.device_id, device_id, sizeof(message.device_id));

    return dispatch_message_async(request, &message);
}

static void command_completion(const dispatch_result_t *result, void *arg)
{
    command_async_context_t *context = arg;
    web_send_dispatch_result(context->request, result);
    if (httpd_req_async_handler_complete(context->request) != ESP_OK) {
        ESP_LOGW(TAG, "Could not complete asynchronous command request");
    }
    free(context);
}

// Shared async dispatch path for every mutating endpoint (Plan v2 §43):
// the HTTPD task only parses and enqueues; executor workers run the
// dispatcher so BLE ACK waits never block HTTP.
static esp_err_t dispatch_message_async(httpd_req_t *request,
                                        const gw_message_t *message)
{
    command_async_context_t *context = malloc(sizeof(*context));
    if (context == NULL) {
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Could not allocate command context");
    }

    esp_err_t error = httpd_req_async_handler_begin(request, &context->request);
    if (error != ESP_OK) {
        free(context);
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Could not start asynchronous dispatch");
    }

    // Queue admission failure answers 503 inline; device-level BUSY still
    // comes back later as 409 via completion.
    if (command_executor_submit(message, command_completion, context) !=
        ESP_OK) {
        web_send_api_error(context->request, "503 Service Unavailable",
                           "Command executor is full");
        httpd_req_async_handler_complete(context->request);
        free(context);
    }
    return ESP_OK;
}

esp_err_t web_device_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/devices", .method = HTTP_GET,
         .handler = devices_get_handler},
        {.uri = "/api/devices", .method = HTTP_POST,
         .handler = devices_write_handler},
        {.uri = "/api/devices", .method = HTTP_PUT,
         .handler = devices_write_handler},
        {.uri = "/api/devices", .method = HTTP_DELETE,
         .handler = devices_delete_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
