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
#include "web_auth.h"
#include "web_auth_http.h"

static const char *TAG = "web_command_api";

typedef struct {
    httpd_req_t *request;
} command_async_context_t;

static const char *http_status_for(const dispatch_result_t *result)
{
    switch (result->status) {
    case DISPATCH_STATUS_OK:
        return "200 OK";
    case DISPATCH_STATUS_INVALID_ARGUMENT:
    case DISPATCH_STATUS_UNSUPPORTED_COMMAND:
    case DISPATCH_STATUS_INVALID_COMMAND_ARGUMENT:
        return "400 Bad Request";
    case DISPATCH_STATUS_NOT_FOUND:
        return "404 Not Found";
    case DISPATCH_STATUS_BUSY:
    case DISPATCH_STATUS_CONFLICT:
        return "409 Conflict";
    case DISPATCH_STATUS_TIMEOUT:
        return "504 Gateway Timeout";
    case DISPATCH_STATUS_RESOURCE_EXHAUSTED:
        return "507 Insufficient Storage";
    case DISPATCH_STATUS_NOT_CONNECTED:
    case DISPATCH_STATUS_TRANSPORT_ERROR:
    case DISPATCH_STATUS_DEVICE_ERROR:
        return "502 Bad Gateway";
    default:
        return "500 Internal Server Error";
    }
}

static const char *error_code_for(const dispatch_result_t *result)
{
    switch (result->status) {
    case DISPATCH_STATUS_INVALID_ARGUMENT: return "invalid_request";
    case DISPATCH_STATUS_NOT_FOUND: return "device_not_found";
    case DISPATCH_STATUS_BUSY: return "device_busy";
    case DISPATCH_STATUS_CONFLICT: return "conflict";
    case DISPATCH_STATUS_RESOURCE_EXHAUSTED: return "store_full";
    case DISPATCH_STATUS_TIMEOUT: return "command_timeout";
    case DISPATCH_STATUS_NOT_CONNECTED: return "device_not_connected";
    case DISPATCH_STATUS_TRANSPORT_ERROR: return "transport_error";
    case DISPATCH_STATUS_DEVICE_ERROR: return "device_error";
    case DISPATCH_STATUS_UNSUPPORTED_COMMAND: return "unsupported_command";
    case DISPATCH_STATUS_INVALID_COMMAND_ARGUMENT:
        return "invalid_command_argument";
    default: return "internal_error";
    }
}

void web_send_dispatch_result(httpd_req_t *request,
                              const dispatch_result_t *result)
{
    bool ok = dispatch_result_is_ok(result);
    if (!ok) httpd_resp_set_status(request, http_status_for(result));

    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddBoolToObject(json, "success", ok);
        cJSON_AddNumberToObject(json, "status", (int)result->status);
        cJSON_AddStringToObject(
            json, "message",
            result->format == DISPATCH_RESULT_TEXT ? result->payload : "");
        if (!ok) {
            cJSON *error = cJSON_AddObjectToObject(json, "error");
            if (error != NULL) {
                cJSON_AddStringToObject(error, "code",
                                        error_code_for(result));
            }
        }
        if (result->format == DISPATCH_RESULT_JSON) {
            cJSON *data = cJSON_Parse(result->payload);
            if (data != NULL) cJSON_AddItemToObject(json, "data", data);
        }
    }
    web_send_json(request, json);
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

    if (command_executor_submit(message, command_completion, context) !=
        ESP_OK) {
        web_send_api_error(context->request, "503 Service Unavailable",
                           "Command executor is full");
        httpd_req_async_handler_complete(context->request);
        free(context);
    }
    return ESP_OK;
}

static esp_err_t command_post_handler(httpd_req_t *request)
{
    web_auth_result_t auth = web_auth_require_request(request);
    if (auth != WEB_AUTH_OK) {
        return web_send_api_error_code(request, "401 Unauthorized",
                                       "Authentication required",
                                       "auth_required");
    }

    char body[WEB_COMMAND_BODY_MAX_LEN];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) {
        return web_send_body_error(request, body_status);
    }

    const char *device_id = web_get_json_string(
        json, "device_id", GW_MSG_DEVICE_ID_LEN, true);
    const char *command = web_get_json_string(
        json, "command", GW_MSG_COMMAND_LEN, true);
    if (device_id == NULL || command == NULL) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                  "device_id and command are required",
                                  "invalid_request");
    }

    gw_message_t message = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .has_device_id = true,
    };
    strlcpy(message.type, "device_command", sizeof(message.type));
    strlcpy(message.device_id, device_id, sizeof(message.device_id));
    strlcpy(message.command, command, sizeof(message.command));

    const cJSON *int_value = cJSON_GetObjectItemCaseSensitive(json, "int_value");
    if (int_value != NULL) {
        if (!cJSON_IsNumber(int_value) ||
            int_value->valuedouble != (double)int_value->valueint) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                      "int_value must be an integer",
                                      "invalid_request");
        }
        message.int_value = int_value->valueint;
        message.has_int_value = true;
    }

    const cJSON *bool_value = cJSON_GetObjectItemCaseSensitive(json, "bool_value");
    if (bool_value != NULL) {
        if (!cJSON_IsBool(bool_value)) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                      "bool_value must be boolean",
                                      "invalid_request");
        }
        message.bool_value = cJSON_IsTrue(bool_value);
        message.has_bool_value = true;
    }
    cJSON_Delete(json);

    return dispatch_message_async(request, &message);
}

esp_err_t web_command_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/command", .method = HTTP_POST,
         .handler = command_post_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
