#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_command_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web_http.h"

static const char *TAG = "web_command_api";
static size_t s_active_contexts;

typedef struct {
    httpd_req_t *request;
} command_async_context_t;

/* ── Device command path (new service) ───────────────────────────────── */

static const char *device_status_http(const device_command_status_t status)
{
    switch (status) {
    case DEVICE_CMD_STATUS_OK:               return "200 OK";
    case DEVICE_CMD_STATUS_INVALID_ARGUMENT: return "400 Bad Request";
    case DEVICE_CMD_STATUS_TYPE_MISMATCH:    return "400 Bad Request";
    case DEVICE_CMD_STATUS_RANGE_ERROR:      return "400 Bad Request";
    case DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND: return "400 Bad Request";
    case DEVICE_CMD_STATUS_SCHEMA_NOT_READY: return "409 Conflict";
    case DEVICE_CMD_STATUS_BUSY:             return "409 Conflict";
    case DEVICE_CMD_STATUS_QUEUE_FULL:       return "503 Service Unavailable";
    case DEVICE_CMD_STATUS_NOT_CONNECTED:    return "502 Bad Gateway";
    case DEVICE_CMD_STATUS_TRANSPORT_ERROR:  return "502 Bad Gateway";
    case DEVICE_CMD_STATUS_TIMEOUT:          return "504 Gateway Timeout";
    case DEVICE_CMD_STATUS_DEVICE_REJECTED:  return "502 Bad Gateway";
    default:                                 return "500 Internal Server Error";
    }
}

static const char *device_error_code(const device_command_status_t status)
{
    switch (status) {
    case DEVICE_CMD_STATUS_INVALID_ARGUMENT: return "invalid_request";
    case DEVICE_CMD_STATUS_TYPE_MISMATCH:    return "type_mismatch";
    case DEVICE_CMD_STATUS_RANGE_ERROR:      return "range_error";
    case DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND: return "unsupported_command";
    case DEVICE_CMD_STATUS_SCHEMA_NOT_READY: return "schema_not_ready";
    case DEVICE_CMD_STATUS_BUSY:             return "device_busy";
    case DEVICE_CMD_STATUS_QUEUE_FULL:       return "service_full";
    case DEVICE_CMD_STATUS_CANCELLED:        return "cancelled";
    case DEVICE_CMD_STATUS_NOT_CONNECTED:    return "device_not_connected";
    case DEVICE_CMD_STATUS_TRANSPORT_ERROR:  return "transport_error";
    case DEVICE_CMD_STATUS_TIMEOUT:          return "command_timeout";
    case DEVICE_CMD_STATUS_DEVICE_REJECTED:  return "device_error";
    default:                                 return "internal_error";
    }
}

static void device_command_completion(const device_command_result_t *result,
                                      void *arg)
{
    command_async_context_t *context = arg;

    bool ok = (result->status == DEVICE_CMD_STATUS_OK);
    if (!ok) {
        httpd_resp_set_status(context->request,
                              device_status_http(result->status));
    }

    cJSON *json = cJSON_CreateObject();
    if (json != NULL) {
        cJSON_AddBoolToObject(json, "success", ok);
        cJSON_AddNumberToObject(json, "status", (int)result->status);
        if (!ok) {
            cJSON *error = cJSON_AddObjectToObject(json, "error");
            if (error != NULL) {
                cJSON_AddStringToObject(error, "code",
                                        device_error_code(result->status));
            }
        }
    }
    web_send_json(context->request, json);

    if (httpd_req_async_handler_complete(context->request) != ESP_OK) {
        ESP_LOGW(TAG, "Could not complete asynchronous device command");
    }
    __atomic_sub_fetch(&s_active_contexts, 1, __ATOMIC_RELAXED);
    free(context);
}

static esp_err_t dispatch_device_command_async(httpd_req_t *request,
                                               const device_command_request_t *req)
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
    __atomic_add_fetch(&s_active_contexts, 1, __ATOMIC_RELAXED);

    if (device_command_service_submit(req, device_command_completion, context) !=
        ESP_OK) {
        web_send_api_error(context->request, "503 Service Unavailable",
                           "Command service is full");
        httpd_req_async_handler_complete(context->request);
        __atomic_sub_fetch(&s_active_contexts, 1, __ATOMIC_RELAXED);
        free(context);
    }
    return ESP_OK;
}

size_t web_command_active_contexts(void)
{
    return __atomic_load_n(&s_active_contexts, __ATOMIC_RELAXED);
}

static esp_err_t command_post_handler(httpd_req_t *request)
{
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

    /* Build typed service request */
    device_command_request_t request_ctx = {0};
    request_ctx.origin = DEVICE_CMD_ORIGIN_CONTROL;
    strlcpy(request_ctx.device_id, device_id, sizeof(request_ctx.device_id));
    strlcpy(request_ctx.command, command, sizeof(request_ctx.command));

    const cJSON *int_value = cJSON_GetObjectItemCaseSensitive(json, "int_value");
    if (int_value != NULL) {
        if (!cJSON_IsNumber(int_value) ||
            int_value->valuedouble != (double)int_value->valueint) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                      "int_value must be an integer",
                                      "invalid_request");
        }
        request_ctx.int_value = int_value->valueint;
        request_ctx.has_int_value = true;
    }

    const cJSON *bool_value = cJSON_GetObjectItemCaseSensitive(json, "bool_value");
    if (bool_value != NULL) {
        if (!cJSON_IsBool(bool_value)) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                      "bool_value must be boolean",
                                      "invalid_request");
        }
        request_ctx.bool_value = cJSON_IsTrue(bool_value);
        request_ctx.has_bool_value = true;
    }
    cJSON_Delete(json);

    return dispatch_device_command_async(request, &request_ctx);
}

esp_err_t web_command_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        WEB_URI_INIT("/api/command", HTTP_POST, command_post_handler),
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
