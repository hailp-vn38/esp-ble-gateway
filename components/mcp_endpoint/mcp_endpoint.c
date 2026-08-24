#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "cbor_codec.h"
#include "command_dispatcher.h"
#include "mcp_endpoint.h"

#define MCP_MAX_REQUEST_LEN 4096

static const char *TAG = "mcp_endpoint";

typedef struct {
    int code;
    const char *message;
} rpc_error_t;

static cJSON *duplicate_id(const cJSON *id)
{
    if (id == NULL || (!cJSON_IsString(id) && !cJSON_IsNumber(id) &&
                       !cJSON_IsNull(id))) {
        return cJSON_CreateNull();
    }
    return cJSON_Duplicate(id, true);
}

static esp_err_t send_json(httpd_req_t *request, cJSON *response)
{
    if (response == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    char *body = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (body == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(request, "application/json");
    esp_err_t error = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return error;
}

static esp_err_t send_rpc_error(httpd_req_t *request, int code,
                                const char *message, const cJSON *id)
{
    cJSON *response = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    cJSON *response_id = duplicate_id(id);
    if (response == NULL || error == NULL || response_id == NULL) {
        cJSON_Delete(response);
        cJSON_Delete(error);
        cJSON_Delete(response_id);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id", response_id);
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(response, "error", error);
    return send_json(request, response);
}

static esp_err_t send_rpc_result(httpd_req_t *request, cJSON *result,
                                 const cJSON *id)
{
    cJSON *response = cJSON_CreateObject();
    cJSON *response_id = duplicate_id(id);
    if (response == NULL || response_id == NULL || result == NULL) {
        cJSON_Delete(response);
        cJSON_Delete(response_id);
        cJSON_Delete(result);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id", response_id);
    cJSON_AddItemToObject(response, "result", result);
    return send_json(request, response);
}

static const char *tool_description(const char *name)
{
    if (strcmp(name, "add_device") == 0) return "Add a BLE device to the gateway";
    if (strcmp(name, "edit_device") == 0) return "Edit a stored device";
    if (strcmp(name, "delete_device") == 0) return "Delete and disconnect a device";
    if (strcmp(name, "list_devices") == 0) return "List devices known by the gateway";
    if (strcmp(name, "get_status") == 0) return "Get gateway and BLE status";
    return "Run a registered gateway command";
}

static cJSON *create_input_schema(void)
{
    cJSON *schema = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    if (schema == NULL || properties == NULL) {
        cJSON_Delete(schema);
        cJSON_Delete(properties);
        return NULL;
    }

    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", properties);
    const char *string_fields[] = {
        "device_id", "name", "device_type", "ble_addr", "type"
    };
    for (size_t i = 0; i < sizeof(string_fields) / sizeof(string_fields[0]); i++) {
        cJSON *field = cJSON_CreateObject();
        if (field == NULL) continue;
        cJSON_AddStringToObject(field, "type", "string");
        cJSON_AddItemToObject(properties, string_fields[i], field);
    }
    cJSON *integer_field = cJSON_CreateObject();
    cJSON *address_type_field = cJSON_CreateObject();
    cJSON *boolean_field = cJSON_CreateObject();
    if (integer_field != NULL) {
        cJSON_AddStringToObject(integer_field, "type", "integer");
        cJSON_AddItemToObject(properties, "int_value", integer_field);
    }
    if (address_type_field != NULL) {
        cJSON_AddStringToObject(address_type_field, "type", "integer");
        cJSON_AddItemToObject(properties, "ble_addr_type", address_type_field);
    }
    if (boolean_field != NULL) {
        cJSON_AddStringToObject(boolean_field, "type", "boolean");
        cJSON_AddItemToObject(properties, "bool_value", boolean_field);
    }
    return schema;
}

static cJSON *handle_list_tools(void)
{
    const char *names[DISPATCHER_MAX_COMMANDS];
    int count = command_dispatcher_get_registered_names(
        names, DISPATCHER_MAX_COMMANDS);
    if (count < 0) return NULL;

    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool_names = cJSON_CreateArray();
    if (result == NULL || tools == NULL || tool_names == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(tools);
        cJSON_Delete(tool_names);
        return NULL;
    }
    cJSON_AddItemToObject(result, "tools", tools);
    cJSON_AddItemToObject(result, "tool_names", tool_names);

    for (int i = 0; i < count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON *schema = create_input_schema();
        cJSON *name_copy = cJSON_CreateString(names[i]);
        if (tool == NULL || schema == NULL || name_copy == NULL) {
            cJSON_Delete(tool);
            cJSON_Delete(schema);
            cJSON_Delete(name_copy);
            cJSON_Delete(result);
            return NULL;
        }
        cJSON_AddStringToObject(tool, "name", names[i]);
        cJSON_AddStringToObject(tool, "description", tool_description(names[i]));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
        cJSON_AddItemToArray(tool_names, name_copy);
    }
    return result;
}

static bool copy_optional_field(cJSON *destination, const cJSON *source,
                                const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(source, name);
    if (item == NULL) return true;
    cJSON *copy = cJSON_Duplicate(item, true);
    if (copy == NULL) return false;
    cJSON_AddItemToObject(destination, name, copy);
    return true;
}

static int params_to_message(const cJSON *params, gw_message_t *message,
                             rpc_error_t *error)
{
    if (!cJSON_IsObject(params)) {
        *error = (rpc_error_t){-32602, "params must be an object"};
        return -1;
    }

    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (arguments != NULL && !cJSON_IsObject(arguments)) {
        *error = (rpc_error_t){-32602, "arguments must be an object"};
        return -1;
    }
    const cJSON *source = cJSON_IsObject(arguments) ? arguments : params;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (name != NULL && !cJSON_IsString(name)) {
        *error = (rpc_error_t){-32602, "tool name must be a string"};
        return -1;
    }
    const cJSON *command = cJSON_IsString(name)
                               ? name
                               : cJSON_GetObjectItemCaseSensitive(source, "command");
    if (!cJSON_IsString(command) || command->valuestring == NULL ||
        command->valuestring[0] == '\0') {
        *error = (rpc_error_t){-32602, "missing or invalid tool name/command"};
        return -1;
    }

    const cJSON *explicit_type = cJSON_GetObjectItemCaseSensitive(source, "type");
    const cJSON *device_id = cJSON_GetObjectItemCaseSensitive(source, "device_id");
    const char *message_type = NULL;
    if (explicit_type != NULL) {
        if (!cJSON_IsString(explicit_type) || explicit_type->valuestring == NULL ||
            (strcmp(explicit_type->valuestring, "gateway_command") != 0 &&
             strcmp(explicit_type->valuestring, "device_command") != 0)) {
            *error = (rpc_error_t){-32602, "invalid command type"};
            return -1;
        }
        message_type = explicit_type->valuestring;
    } else if (command_dispatcher_is_registered(command->valuestring)) {
        message_type = "gateway_command";
    } else if (cJSON_IsString(device_id) && device_id->valuestring != NULL &&
               device_id->valuestring[0] != '\0') {
        message_type = "device_command";
    } else {
        message_type = "gateway_command";
    }

    cJSON *normalized = cJSON_CreateObject();
    if (normalized == NULL) {
        *error = (rpc_error_t){-32603, "out of memory"};
        return -1;
    }
    cJSON_AddStringToObject(normalized, "type", message_type);
    cJSON_AddStringToObject(normalized, "command", command->valuestring);
    const char *optional_fields[] = {
        "protocol_version", "device_id", "int_value", "bool_value", "name",
        "device_type", "ble_addr", "ble_addr_type"
    };
    bool copied = true;
    for (size_t i = 0; i < sizeof(optional_fields) / sizeof(optional_fields[0]); i++) {
        copied = copied && copy_optional_field(normalized, source, optional_fields[i]);
    }

    char *json = copied ? cJSON_PrintUnformatted(normalized) : NULL;
    cJSON_Delete(normalized);
    if (json == NULL) {
        *error = (rpc_error_t){-32603, "out of memory"};
        return -1;
    }
    int result = cbor_codec_json_to_msg(json, message);
    cJSON_free(json);
    if (result != 0) {
        *error = (rpc_error_t){-32602, "invalid tool arguments"};
        return -1;
    }
    return 0;
}

static cJSON *handle_call_tool(const cJSON *params, rpc_error_t *error)
{
    gw_message_t message;
    if (params_to_message(params, &message, error) != 0) return NULL;

    dispatch_result_t dispatch_result;
    command_dispatcher_handle(&message, &dispatch_result);

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        *error = (rpc_error_t){-32603, "out of memory"};
        return NULL;
    }
    cJSON_AddBoolToObject(result, "success", dispatch_result.success != 0);
    cJSON_AddStringToObject(result, "message", dispatch_result.message);

    cJSON *data = cJSON_Parse(dispatch_result.message);
    if (data != NULL) cJSON_AddItemToObject(result, "data", data);
    return result;
}

static char *receive_body(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > MCP_MAX_REQUEST_LEN) {
        return NULL;
    }
    char *body = malloc((size_t)request->content_len + 1);
    if (body == NULL) return NULL;

    size_t received = 0;
    while (received < (size_t)request->content_len) {
        int chunk = httpd_req_recv(request, body + received,
                                   request->content_len - (int)received);
        if (chunk == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (chunk <= 0) {
            free(body);
            return NULL;
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';
    return body;
}

static esp_err_t mcp_post_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > MCP_MAX_REQUEST_LEN) {
        return send_rpc_error(request, -32600, "Invalid Request", NULL);
    }
    char *body = receive_body(request);
    if (body == NULL) {
        return send_rpc_error(request, -32700, "Parse error", NULL);
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_rpc_error(request, -32700, "Parse error", NULL);
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "jsonrpc");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    bool valid_id = id == NULL || cJSON_IsString(id) || cJSON_IsNumber(id) ||
                    cJSON_IsNull(id);
    if (!cJSON_IsString(version) || version->valuestring == NULL ||
        strcmp(version->valuestring, "2.0") != 0 || !cJSON_IsString(method) ||
        method->valuestring == NULL || method->valuestring[0] == '\0' || !valid_id) {
        esp_err_t result = send_rpc_error(request, -32600, "Invalid Request", id);
        cJSON_Delete(root);
        return result;
    }

    bool notification = id == NULL;
    cJSON *rpc_result = NULL;
    rpc_error_t rpc_error = {0};
    if (strcmp(method->valuestring, "list_tools") == 0 ||
        strcmp(method->valuestring, "tools/list") == 0) {
        rpc_result = handle_list_tools();
        if (rpc_result == NULL) rpc_error = (rpc_error_t){-32603, "Internal error"};
    } else if (strcmp(method->valuestring, "call_tool") == 0 ||
               strcmp(method->valuestring, "tools/call") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        rpc_result = handle_call_tool(params, &rpc_error);
    } else {
        rpc_error = (rpc_error_t){-32601, "Method not found"};
    }

    if (notification) {
        cJSON_Delete(rpc_result);
        cJSON_Delete(root);
        httpd_resp_set_status(request, "204 No Content");
        return httpd_resp_send(request, NULL, 0);
    }

    esp_err_t result;
    if (rpc_error.code != 0) {
        cJSON_Delete(rpc_result);
        result = send_rpc_error(request, rpc_error.code, rpc_error.message, id);
    } else {
        result = send_rpc_result(request, rpc_result, id);
    }
    cJSON_Delete(root);
    return result;
}

int mcp_endpoint_register(httpd_handle_t server)
{
    if (server == NULL) return -1;
    const httpd_uri_t route = {
        .uri = "/mcp",
        .method = HTTP_POST,
        .handler = mcp_post_handler,
        .user_ctx = NULL,
    };
    esp_err_t error = httpd_register_uri_handler(server, &route);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not register POST /mcp: %s", esp_err_to_name(error));
        return -1;
    }
    ESP_LOGI(TAG, "MCP JSON-RPC endpoint registered at POST /mcp");
    return 0;
}
