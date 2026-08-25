#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "command_dispatcher.h"
#include "web_http.h"

#define COMMAND_WORKER_COUNT 3
#define COMMAND_WORKER_STACK 8192

static const char *TAG = "web_gateway_api";
static SemaphoreHandle_t s_command_slots;

typedef struct {
    httpd_req_t *request;
    gw_message_t message;
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

static const char *http_status_for(const dispatch_result_t *result)
{
    switch (result->status) {
    case DISPATCH_STATUS_OK:
        return "200 OK";
    case DISPATCH_STATUS_INVALID_ARGUMENT:
        return "400 Bad Request";
    case DISPATCH_STATUS_NOT_FOUND:
        return "404 Not Found";
    case DISPATCH_STATUS_BUSY:
        return "409 Conflict";
    case DISPATCH_STATUS_TIMEOUT:
        return "504 Gateway Timeout";
    case DISPATCH_STATUS_NOT_CONNECTED:
    case DISPATCH_STATUS_TRANSPORT_ERROR:
    case DISPATCH_STATUS_DEVICE_ERROR:
        return "502 Bad Gateway";
    default:
        return "500 Internal Server Error";
    }
}

static esp_err_t send_dispatch_result(httpd_req_t *request,
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
        if (result->format == DISPATCH_RESULT_JSON) {
            cJSON *data = cJSON_Parse(result->payload);
            if (data != NULL) cJSON_AddItemToObject(json, "data", data);
        }
    }
    return web_send_json(request, json);
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
    if (cJSON_GetObjectItemCaseSensitive(json, "type") != NULL) {
        const char *type = web_get_json_string(
            json, "type", sizeof(message->device_type), true);
        if (type == NULL) return -1;
        strlcpy(message->device_type, type, sizeof(message->device_type));
    }
    if (require_metadata && message->name[0] == '\0') {
        strlcpy(message->name, message->device_id, sizeof(message->name));
    }
    if (require_metadata && message->device_type[0] == '\0') {
        strlcpy(message->device_type, "generic", sizeof(message->device_type));
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

static esp_err_t devices_write_handler(httpd_req_t *request)
{
    char body[WEB_REQUEST_BODY_MAX_LEN];
    cJSON *json = web_parse_request_json(request, body, sizeof(body));
    if (json == NULL) {
        return web_send_api_error(request, "400 Bad Request", "Invalid JSON body");
    }

    const char *command =
        request->method == HTTP_POST ? "add_device" : "edit_device";
    gw_message_t message;
    int parse_result = fill_device_message(json, &message, command,
                                           request->method == HTTP_POST);
    cJSON_Delete(json);
    if (parse_result != 0) {
        return web_send_api_error(request, "400 Bad Request",
                                  "Invalid device fields");
    }

    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    return send_dispatch_result(request, &result);
}

static esp_err_t devices_delete_handler(httpd_req_t *request)
{
    char query[128];
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "device_id", device_id,
                              sizeof(device_id)) != ESP_OK ||
        device_id[0] == '\0') {
        return web_send_api_error(request, "400 Bad Request", "Missing device_id");
    }

    gw_message_t message = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .has_device_id = true,
    };
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, "delete_device", sizeof(message.command));
    strlcpy(message.device_id, device_id, sizeof(message.device_id));

    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    return send_dispatch_result(request, &result);
}

static void command_http_worker(void *arg)
{
    command_async_context_t *context = arg;
    dispatch_result_t result;
    command_dispatcher_handle(&context->message, &result);
    send_dispatch_result(context->request, &result);
    if (httpd_req_async_handler_complete(context->request) != ESP_OK) {
        ESP_LOGW(TAG, "Could not complete asynchronous command request");
    }
    free(context);
    xSemaphoreGive(s_command_slots);
    vTaskDelete(NULL);
}

static esp_err_t command_post_handler(httpd_req_t *request)
{
    char body[WEB_REQUEST_BODY_MAX_LEN];
    cJSON *json = web_parse_request_json(request, body, sizeof(body));
    if (json == NULL) {
        return web_send_api_error(request, "400 Bad Request", "Invalid JSON body");
    }

    const char *device_id = web_get_json_string(
        json, "device_id", GW_MSG_DEVICE_ID_LEN, true);
    const char *command = web_get_json_string(
        json, "command", GW_MSG_COMMAND_LEN, true);
    if (device_id == NULL || command == NULL) {
        cJSON_Delete(json);
        return web_send_api_error(request, "400 Bad Request",
                                  "device_id and command are required");
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
            return web_send_api_error(request, "400 Bad Request",
                                      "int_value must be an integer");
        }
        message.int_value = int_value->valueint;
    }

    const cJSON *bool_value = cJSON_GetObjectItemCaseSensitive(json, "bool_value");
    if (bool_value != NULL) {
        if (!cJSON_IsBool(bool_value)) {
            cJSON_Delete(json);
            return web_send_api_error(request, "400 Bad Request",
                                      "bool_value must be boolean");
        }
        message.bool_value = cJSON_IsTrue(bool_value);
    }
    cJSON_Delete(json);

    if (s_command_slots == NULL || xSemaphoreTake(s_command_slots, 0) != pdTRUE) {
        return web_send_api_error(request, "503 Service Unavailable",
                                  "All command workers are busy");
    }

    command_async_context_t *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        xSemaphoreGive(s_command_slots);
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Could not allocate command worker");
    }
    context->message = message;

    esp_err_t error = httpd_req_async_handler_begin(request, &context->request);
    if (error != ESP_OK) {
        free(context);
        xSemaphoreGive(s_command_slots);
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Could not start asynchronous command");
    }

    if (xTaskCreate(command_http_worker, "http_command", COMMAND_WORKER_STACK,
                    context, 5, NULL) != pdPASS) {
        web_send_api_error(context->request, "503 Service Unavailable",
                           "Could not start command worker");
        httpd_req_async_handler_complete(context->request);
        free(context);
        xSemaphoreGive(s_command_slots);
    }
    return ESP_OK;
}

esp_err_t web_gateway_api_init(void)
{
    if (s_command_slots == NULL) {
        s_command_slots = xSemaphoreCreateCounting(COMMAND_WORKER_COUNT,
                                                   COMMAND_WORKER_COUNT);
    }
    return s_command_slots != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t web_gateway_api_register(httpd_handle_t server)
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
        {.uri = "/api/command", .method = HTTP_POST,
         .handler = command_post_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
