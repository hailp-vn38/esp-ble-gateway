#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"

#include "cbor_codec.h"
#include "command_dispatcher.h"
#include "mcp_endpoint_internal.h"

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

cJSON *mcp_tools_list(void)
{
    char names[DISPATCHER_MAX_COMMANDS][GW_MSG_COMMAND_LEN];
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
                             mcp_rpc_error_t *error)
{
    if (!cJSON_IsObject(params)) {
        *error = (mcp_rpc_error_t){-32602, "params must be an object"};
        return -1;
    }

    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (arguments != NULL && !cJSON_IsObject(arguments)) {
        *error = (mcp_rpc_error_t){-32602, "arguments must be an object"};
        return -1;
    }
    const cJSON *source = cJSON_IsObject(arguments) ? arguments : params;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (name != NULL && !cJSON_IsString(name)) {
        *error = (mcp_rpc_error_t){-32602, "tool name must be a string"};
        return -1;
    }
    const cJSON *command = cJSON_IsString(name)
                               ? name
                               : cJSON_GetObjectItemCaseSensitive(source, "command");
    if (!cJSON_IsString(command) || command->valuestring == NULL ||
        command->valuestring[0] == '\0') {
        *error = (mcp_rpc_error_t){-32602, "missing or invalid tool name/command"};
        return -1;
    }

    const cJSON *explicit_type = cJSON_GetObjectItemCaseSensitive(source, "type");
    const cJSON *device_id = cJSON_GetObjectItemCaseSensitive(source, "device_id");
    const char *message_type = NULL;
    if (explicit_type != NULL) {
        if (!cJSON_IsString(explicit_type) || explicit_type->valuestring == NULL ||
            (strcmp(explicit_type->valuestring, "gateway_command") != 0 &&
             strcmp(explicit_type->valuestring, "device_command") != 0)) {
            *error = (mcp_rpc_error_t){-32602, "invalid command type"};
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
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
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
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return -1;
    }
    int result = cbor_codec_json_to_msg(json, message);
    cJSON_free(json);
    if (result != 0) {
        *error = (mcp_rpc_error_t){-32602, "invalid tool arguments"};
        return -1;
    }
    return 0;
}

cJSON *mcp_tools_call(const cJSON *params, mcp_rpc_error_t *error)
{
    gw_message_t message;
    if (params_to_message(params, &message, error) != 0) return NULL;

    dispatch_result_t dispatch_result;
    command_dispatcher_handle(&message, &dispatch_result);

    bool ok = dispatch_result_is_ok(&dispatch_result);
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }
    cJSON_AddBoolToObject(result, "success", ok);
    cJSON_AddNumberToObject(result, "status", (int)dispatch_result.status);
    cJSON_AddStringToObject(
        result, "message",
        dispatch_result.format == DISPATCH_RESULT_TEXT ? dispatch_result.payload
                                                       : "");

    if (dispatch_result.format == DISPATCH_RESULT_JSON) {
        cJSON *data = cJSON_Parse(dispatch_result.payload);
        if (data != NULL) cJSON_AddItemToObject(result, "data", data);
        else {
            cJSON_Delete(result);
            *error = (mcp_rpc_error_t){-32603, "dispatcher returned invalid JSON"};
            return NULL;
        }
    }
    return result;
}
