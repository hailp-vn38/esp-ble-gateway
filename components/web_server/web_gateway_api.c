#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "command_dispatcher.h"
#include "command_executor.h"
#include "device_capabilities.h"
#include "ble_central.h"
#include "web_http.h"
#include "web_admin_auth.h"
#include "mcp_tool_exposure.h"

static const char *TAG = "web_gateway_api";

typedef struct {
    httpd_req_t *request;
} command_async_context_t;

static esp_err_t dispatch_message_async(httpd_req_t *request,
                                        const gw_message_t *message);

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
        if (!ok) {
            // Machine-readable code (Plan v2 §51-§52); message stays put for
            // backward compatibility with the dashboard.
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
    send_dispatch_result(context->request, result);
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

static esp_err_t capabilities_get_handler(httpd_req_t *request)
{
    char query[128];
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        web_get_query_value(query, "device_id", device_id,
                            sizeof(device_id)) != ESP_OK ||
        device_id[0] == '\0') {
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }
    gw_message_t message = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .has_device_id = true,
    };
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, "list_device_capabilities",
            sizeof(message.command));
    strlcpy(message.device_id, device_id, sizeof(message.device_id));
    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    return send_dispatch_result(request, &result);
}

static esp_err_t capabilities_refresh_handler(httpd_req_t *request)
{
    char body[WEB_COMMAND_BODY_MAX_LEN];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) return web_send_body_error(request, body_status);
    const char *device_id = web_get_json_string(
        json, "device_id", GW_MSG_DEVICE_ID_LEN, true);
    if (device_id == NULL) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }

    /* Preflight: device exists? */
    device_capability_snapshot_t snapshot;
    esp_err_t snapshot_error = device_capabilities_get(device_id, &snapshot);
    if (snapshot_error == ESP_ERR_NOT_FOUND) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "404 Not Found",
                                       "Device not found", "device_not_found");
    }

    /* Preflight: BLE runtime ready? */
    ble_central_device_status_t ble_status;
    bool ble_ready =
        ble_central_get_device_status(device_id, &ble_status) == BLE_CENTRAL_OK &&
        ble_status.ready;
    if (!ble_ready) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "502 Bad Gateway",
                                       "Device is not BLE ready",
                                       "device_not_connected");
    }

    uint32_t generation = 0;
    esp_err_t error = device_capabilities_refresh(device_id, &generation);
    cJSON_Delete(json);
    if (error == ESP_ERR_NOT_FOUND) {
        return web_send_api_error_code(request, "404 Not Found",
                                       "Device not found", "device_not_found");
    }
    if (error == ESP_ERR_INVALID_STATE) {
        return web_send_api_error_code(request, "409 Conflict",
                                       "Capability operation already in progress",
                                       "device_busy");
    }
    if (error != ESP_OK) {
        return web_send_api_error_code(request, "503 Service Unavailable",
                                       "Capability refresh queue is full",
                                       "queue_full");
    }

    httpd_resp_set_status(request, "202 Accepted");
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "accepted", true);
    cJSON_AddStringToObject(response, "device_id", device_id);
    cJSON_AddNumberToObject(response, "generation", generation);
    return web_send_json(request, response);
}

esp_err_t web_gateway_api_init(void)
{
    return ESP_OK;
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
        {.uri = "/api/capabilities", .method = HTTP_GET,
         .handler = capabilities_get_handler},
        {.uri = "/api/capabilities/refresh", .method = HTTP_POST,
         .handler = capabilities_refresh_handler},
    };
    esp_err_t err = web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
    if (err != ESP_OK) return err;

    // Exposure API routes (§27.6 — separate registration for budget tracking).
    return web_exposure_api_register(server);
}

// ---------------------------------------------------------------------------
// GET /api/mcp/exposures — admin-protected (§27.3)
// ---------------------------------------------------------------------------

static esp_err_t exposure_get_handler(httpd_req_t *request)
{
    web_admin_auth_result_t auth = web_admin_auth_check(request);
    if (auth != WEB_ADMIN_AUTH_OK) {
        if (auth == WEB_ADMIN_AUTH_NOT_CONFIGURED) {
            return web_send_api_error_code(request, "403 Forbidden",
                                           "Admin auth not configured",
                                           "admin_auth_not_configured");
        }
        return web_send_api_error_code(request, "401 Unauthorized",
                                       "Invalid admin token",
                                       "unauthorized");
    }

    char query[128];
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        web_get_query_value(query, "device_id", device_id,
                            sizeof(device_id)) != ESP_OK ||
        device_id[0] == '\0') {
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }

    // Get capability snapshot.
    device_capability_snapshot_t cap;
    esp_err_t cap_err = device_capabilities_get(device_id, &cap);

    // Get exposure capacity.
    mcp_exposure_capacity_t capacity;
    mcp_tool_exposure_get_capacity(&capacity);

    // Get catalog revision.
    uint32_t revision = mcp_tool_catalog_get_revision();

    cJSON *root = cJSON_CreateObject();
    cJSON *commands = cJSON_CreateArray();
    if (root == NULL || commands == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(commands);
        return web_send_api_error(request, "500 Internal Server Error",
                                  "Out of memory");
    }

    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddNumberToObject(root, "catalog_revision", revision);

    cJSON *cap_obj = cJSON_CreateObject();
    if (cap_obj != NULL) {
        cJSON_AddNumberToObject(cap_obj, "enabled", capacity.enabled);
        cJSON_AddNumberToObject(cap_obj, "max_enabled", capacity.max_enabled);
        cJSON_AddNumberToObject(cap_obj, "records", capacity.records);
        cJSON_AddNumberToObject(cap_obj, "max_records", capacity.max_records);
        cJSON_AddItemToObject(root, "capacity", cap_obj);
    }

    if (cap_err == ESP_OK && cap.has_committed && cap.count > 0) {
        for (size_t i = 0; i < cap.count; i++) {
            const device_capability_t *c = &cap.items[i];
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) continue;

            cJSON_AddStringToObject(item, "command", c->command);
            cJSON_AddStringToObject(item, "label", c->label);

            const char *vt_str = c->value_type == DEVICE_CAP_VALUE_BOOL
                                     ? "boolean"
                                     : c->value_type == DEVICE_CAP_VALUE_INT
                                           ? "integer"
                                           : "none";
            cJSON_AddStringToObject(item, "value_type", vt_str);
            cJSON_AddBoolToObject(item, "destructive",
                                  (c->flags & DEVICE_CAP_FLAG_DESTRUCTIVE) != 0);
            cJSON_AddBoolToObject(item, "idempotent",
                                  (c->flags & DEVICE_CAP_FLAG_IDEMPOTENT) != 0);
            if (c->value_type == DEVICE_CAP_VALUE_INT) {
                cJSON_AddNumberToObject(item, "minimum", c->min_value);
                cJSON_AddNumberToObject(item, "maximum", c->max_value);
                cJSON_AddNumberToObject(item, "step", c->step);
                cJSON_AddStringToObject(item, "unit", c->unit);
            }

            // Check exposure state.
            mcp_tool_exposure_t exposure;
            if (mcp_tool_exposure_get(device_id, c->command,
                                      &exposure) == ESP_OK) {
                cJSON_AddBoolToObject(item, "enabled", true);
                const char *state_str =
                    exposure.state == MCP_EXPOSURE_ENABLED
                        ? "enabled"
                        : exposure.state == MCP_EXPOSURE_NEEDS_REVIEW
                              ? "needs_review"
                              : "orphaned";
                cJSON_AddStringToObject(item, "state", state_str);
                cJSON_AddStringToObject(item, "tool_name", exposure.tool_name);
            } else {
                cJSON_AddBoolToObject(item, "enabled", false);
                cJSON_AddStringToObject(item, "state", "disabled");
                // Generate tool name deterministically.
                char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
                if (mcp_tool_name_generate(device_id, c->command,
                                           tool_name,
                                           sizeof(tool_name)) == ESP_OK) {
                    cJSON_AddStringToObject(item, "tool_name", tool_name);
                }
            }

            cJSON_AddItemToArray(commands, item);
        }
    }

    cJSON_AddItemToObject(root, "commands", commands);

    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return web_send_json(request, root);
}

// ---------------------------------------------------------------------------
// PUT /api/mcp/exposures — admin-protected, single/bulk (§27.4)
// ---------------------------------------------------------------------------

static esp_err_t exposure_put_handler(httpd_req_t *request)
{
    web_admin_auth_result_t auth = web_admin_auth_check(request);
    if (auth != WEB_ADMIN_AUTH_OK) {
        if (auth == WEB_ADMIN_AUTH_NOT_CONFIGURED) {
            return web_send_api_error_code(request, "403 Forbidden",
                                           "Admin auth not configured",
                                           "admin_auth_not_configured");
        }
        return web_send_api_error_code(request, "401 Unauthorized",
                                       "Invalid admin token",
                                       "unauthorized");
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
    if (device_id == NULL) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }

    // Single command mode.
    const cJSON *command_item = cJSON_GetObjectItemCaseSensitive(json, "command");
    if (command_item != NULL) {
        const char *command = cJSON_IsString(command_item)
                                  ? command_item->valuestring
                                  : NULL;
        if (command == NULL || command[0] == '\0') {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                           "Invalid command",
                                           "invalid_request");
        }

        const cJSON *enabled_item =
            cJSON_GetObjectItemCaseSensitive(json, "enabled");
        if (!cJSON_IsBool(enabled_item)) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                           "enabled must be boolean",
                                           "invalid_request");
        }

        bool enabled = cJSON_IsTrue(enabled_item);
        esp_err_t err;
        if (enabled) {
            mcp_exposure_enable_options_t opts = {0};
            const cJSON *confirm =
                cJSON_GetObjectItemCaseSensitive(json, "confirm_destructive");
            opts.confirm_destructive =
                cJSON_IsBool(confirm) && cJSON_IsTrue(confirm);
            err = mcp_tool_exposure_enable(device_id, command, &opts);
        } else {
            err = mcp_tool_exposure_disable(device_id, command);
        }

        cJSON_Delete(json);

        if (err == ESP_ERR_NOT_FOUND) {
            return web_send_api_error_code(request, "404 Not Found",
                                           "Device or command not found",
                                           "not_found");
        }
        if (err == ESP_ERR_INVALID_STATE) {
            return web_send_api_error_code(request, "409 Conflict",
                                           "No committed capabilities",
                                           "capabilities_not_ready");
        }
        if (err == ESP_ERR_NO_MEM) {
            return web_send_api_error_code(request, "409 Conflict",
                                           "Capacity exceeded",
                                           "mcp_tool_capacity_exceeded");
        }
        if (err == ESP_ERR_NOT_SUPPORTED) {
            return web_send_api_error_code(request, "403 Forbidden",
                                           "Destructive command blocked by policy",
                                           "destructive_blocked");
        }
        if (err == ESP_ERR_INVALID_ARG) {
            return web_send_api_error_code(request, "400 Bad Request",
                                           "Invalid request",
                                           "invalid_request");
        }
        if (err != ESP_OK) {
            return web_send_api_error(request, "500 Internal Server Error",
                                      "Enable/disable failed");
        }

        cJSON *response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        return web_send_json(request, response);
    }

    // Bulk mode.
    const cJSON *commands_item = cJSON_GetObjectItemCaseSensitive(json, "commands");
    if (commands_item != NULL && cJSON_IsArray(commands_item)) {
        int array_size = cJSON_GetArraySize(commands_item);
        cJSON *response = cJSON_CreateObject();
        cJSON *results = cJSON_CreateArray();
        bool any_error = false;

        for (int i = 0; i < array_size; i++) {
            cJSON *cmd_obj = cJSON_GetArrayItem(commands_item, i);
            const char *cmd = web_get_json_string(cmd_obj, "command",
                                                  GW_MSG_COMMAND_LEN, true);
            const cJSON *en = cJSON_GetObjectItemCaseSensitive(cmd_obj, "enabled");
            if (cmd == NULL || !cJSON_IsBool(en)) {
                cJSON *err_item = cJSON_CreateObject();
                cJSON_AddNumberToObject(err_item, "index", i);
                cJSON_AddStringToObject(err_item, "error", "invalid_request");
                cJSON_AddItemToArray(results, err_item);
                any_error = true;
                continue;
            }

            esp_err_t err;
            if (cJSON_IsTrue(en)) {
                mcp_exposure_enable_options_t opts = {0};
                const cJSON *confirm =
                    cJSON_GetObjectItemCaseSensitive(cmd_obj, "confirm_destructive");
                opts.confirm_destructive =
                    cJSON_IsBool(confirm) && cJSON_IsTrue(confirm);
                err = mcp_tool_exposure_enable(device_id, cmd, &opts);
            } else {
                err = mcp_tool_exposure_disable(device_id, cmd);
            }

            cJSON *result_item = cJSON_CreateObject();
            cJSON_AddStringToObject(result_item, "command", cmd);
            cJSON_AddBoolToObject(result_item, "success", err == ESP_OK);
            if (err != ESP_OK) {
                cJSON_AddStringToObject(result_item, "error",
                                        esp_err_to_name(err));
                any_error = true;
            }
            cJSON_AddItemToArray(results, result_item);
        }

        cJSON_AddItemToObject(response, "results", results);
        cJSON_AddBoolToObject(response, "success", !any_error);

        cJSON_Delete(json);
        httpd_resp_set_status(request, any_error ? "409 Conflict" : "200 OK");
        return web_send_json(request, response);
    }

    cJSON_Delete(json);
    return web_send_api_error_code(request, "400 Bad Request",
                                   "Provide 'command' or 'commands' array",
                                   "invalid_request");
}

// ---------------------------------------------------------------------------
// Exposure API route registration
// ---------------------------------------------------------------------------

esp_err_t web_exposure_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/mcp/exposures", .method = HTTP_GET,
         .handler = exposure_get_handler},
        {.uri = "/api/mcp/exposures", .method = HTTP_PUT,
         .handler = exposure_put_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
