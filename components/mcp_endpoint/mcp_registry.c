#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "sdkconfig.h"

#include "mcp_endpoint_internal.h"

// Strict tool registry: the only tools exposed over MCP. tools/list is built
// exclusively from this table, closing the hidden command surface that the
// old unknown-tool fallback allowed.

#define MAX_LEN_OF(field) ((int)(sizeof(((gw_message_t *)0)->field) - 1))

static bool add_string_field(cJSON *properties, const char *name, int max_length,
                             const char *description)
{
    cJSON *field = cJSON_CreateObject();
    if (field == NULL) return false;
    cJSON_AddStringToObject(field, "type", "string");
    if (max_length > 0) cJSON_AddNumberToObject(field, "maxLength", max_length);
    if (description != NULL) {
        cJSON_AddStringToObject(field, "description", description);
    }
    cJSON_AddItemToObject(properties, name, field);
    return true;
}

static cJSON *new_object_schema(void)
{
    cJSON *schema = cJSON_CreateObject();
    if (schema == NULL) return NULL;
    cJSON *properties = cJSON_CreateObject();
    if (properties == NULL) {
        cJSON_Delete(schema);
        return NULL;
    }
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", properties);
    return schema;
}

// Every partial-failure path frees the whole tree; cJSON_Delete(NULL) is a
// no-op so each helper can bail with {cJSON_Delete(root); return NULL;}
#define SCHEMA_FAIL(schema)       \
    do {                          \
        cJSON_Delete(schema);     \
        return NULL;              \
    } while (0)

static cJSON *schema_add_device(void)
{
    cJSON *schema = new_object_schema();
    if (schema == NULL) return NULL;
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (!add_string_field(properties, "device_id", MAX_LEN_OF(device_id),
                          "Unique device identifier") ||
        !add_string_field(properties, "name", MAX_LEN_OF(name), NULL) ||
        !add_string_field(properties, "device_type",
                          MAX_LEN_OF(device_type), NULL)) {
        SCHEMA_FAIL(schema);
    }
    cJSON *required = cJSON_CreateArray();
    if (required == NULL) SCHEMA_FAIL(schema);
    cJSON_AddItemToArray(required, cJSON_CreateString("device_id"));
    cJSON_AddItemToObject(schema, "required", required);

    cJSON *addr = cJSON_CreateObject();
    if (addr == NULL) SCHEMA_FAIL(schema);
    cJSON_AddStringToObject(addr, "type", "string");
    cJSON_AddStringToObject(addr, "pattern",
                            "^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$");
    cJSON_AddStringToObject(addr, "description", "BLE address (AA:BB:CC:DD:EE:FF)");
    cJSON_AddItemToObject(properties, "ble_addr", addr);

    cJSON *addr_type = cJSON_CreateObject();
    if (addr_type == NULL) SCHEMA_FAIL(schema);
    cJSON_AddStringToObject(addr_type, "type", "integer");
    cJSON_AddNumberToObject(addr_type, "minimum", 0);
    cJSON_AddNumberToObject(addr_type, "maximum", 1);
    cJSON_AddStringToObject(addr_type, "description",
                            "0 = public address, 1 = random address");
    cJSON_AddItemToObject(properties, "ble_addr_type", addr_type);
    return schema;
}

static cJSON *schema_edit_device(void)
{
    cJSON *schema = new_object_schema();
    if (schema == NULL) return NULL;
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (!add_string_field(properties, "device_id", MAX_LEN_OF(device_id),
                          "Device to edit") ||
        !add_string_field(properties, "name", MAX_LEN_OF(name), NULL) ||
        !add_string_field(properties, "device_type",
                          MAX_LEN_OF(device_type), NULL)) {
        SCHEMA_FAIL(schema);
    }
    // At least one editable field must be present.
    cJSON *any_of = cJSON_CreateArray();
    cJSON *branch_name = cJSON_CreateObject();
    cJSON *branch_type = cJSON_CreateObject();
    if (any_of == NULL || branch_name == NULL || branch_type == NULL) {
        cJSON_Delete(any_of);
        cJSON_Delete(branch_name);
        cJSON_Delete(branch_type);
        SCHEMA_FAIL(schema);
    }
    cJSON *req_name = cJSON_CreateArray();
    cJSON *req_type = cJSON_CreateArray();
    if (req_name == NULL || req_type == NULL) {
        cJSON_Delete(req_name);
        cJSON_Delete(req_type);
        SCHEMA_FAIL(schema);
    }
    cJSON_AddItemToArray(req_name, cJSON_CreateString("name"));
    cJSON_AddItemToArray(req_type, cJSON_CreateString("device_type"));
    cJSON_AddItemToObject(branch_name, "required", req_name);
    cJSON_AddItemToObject(branch_type, "required", req_type);
    cJSON_AddItemToArray(any_of, branch_name);
    cJSON_AddItemToArray(any_of, branch_type);
    cJSON_AddItemToObject(schema, "anyOf", any_of);
    return schema;
}

static cJSON *schema_delete_device(void)
{
    cJSON *schema = new_object_schema();
    if (schema == NULL) return NULL;
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (!add_string_field(properties, "device_id", MAX_LEN_OF(device_id),
                          "Device to delete and disconnect")) {
        SCHEMA_FAIL(schema);
    }
    cJSON *required = cJSON_CreateArray();
    if (required == NULL) SCHEMA_FAIL(schema);
    cJSON_AddItemToArray(required, cJSON_CreateString("device_id"));
    cJSON_AddItemToObject(schema, "required", required);
    return schema;
}

static cJSON *schema_device_id(void)
{
    cJSON *schema = new_object_schema();
    if (schema == NULL) return NULL;
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (!add_string_field(properties, "device_id", MAX_LEN_OF(device_id),
                          "Target device identifier")) {
        SCHEMA_FAIL(schema);
    }
    cJSON *required = cJSON_CreateArray();
    if (required == NULL) SCHEMA_FAIL(schema);
    cJSON_AddItemToArray(required, cJSON_CreateString("device_id"));
    cJSON_AddItemToObject(schema, "required", required);
    return schema;
}

static cJSON *schema_empty(void)
{
    return new_object_schema();
}

static cJSON *schema_device_command(void)
{
    cJSON *schema = new_object_schema();
    if (schema == NULL) return NULL;
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (!add_string_field(
            properties, "device_id", MAX_LEN_OF(device_id),
            "Target device identifier") ||
        !add_string_field(properties, "command", MAX_LEN_OF(command),
                          CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST[0] != '\0'
                              ? "Allowed commands: " CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST
                              : "No commands are allowlisted")) {
        SCHEMA_FAIL(schema);
    }
    cJSON *int_value = cJSON_CreateObject();
    cJSON *bool_value = cJSON_CreateObject();
    if (int_value == NULL || bool_value == NULL) {
        cJSON_Delete(int_value);
        cJSON_Delete(bool_value);
        SCHEMA_FAIL(schema);
    }
    cJSON_AddStringToObject(int_value, "type", "integer");
    cJSON_AddStringToObject(int_value, "description",
                            "Integer command argument");
    cJSON_AddItemToObject(properties, "int_value", int_value);
    cJSON_AddStringToObject(bool_value, "type", "boolean");
    cJSON_AddStringToObject(bool_value, "description",
                            "Boolean command argument");
    cJSON_AddItemToObject(properties, "bool_value", bool_value);

    cJSON *required = cJSON_CreateArray();
    if (required == NULL) SCHEMA_FAIL(schema);
    cJSON_AddItemToArray(required, cJSON_CreateString("device_id"));
    cJSON_AddItemToArray(required, cJSON_CreateString("command"));
    cJSON_AddItemToObject(schema, "required", required);
    return schema;
}

#undef SCHEMA_FAIL

static const mcp_tool_desc_t MCP_TOOL_TABLE[] = {
    {"add_device", "Add a BLE device to the gateway", schema_add_device, false, false},
    {"edit_device", "Edit a stored device", schema_edit_device, false, false},
    {"delete_device", "Delete and disconnect a device", schema_delete_device, false, true},
    {"list_devices", "List devices known by the gateway", schema_empty, true, false},
    {"get_status", "Get gateway and BLE status", schema_empty, true, false},
    {"list_device_capabilities", "List commands advertised by a BLE device",
     schema_device_id, true, false},
    {"device_command", "Send an allowlisted command to a device",
     schema_device_command, false, false},
};

const mcp_tool_desc_t *mcp_registry_find(const char *name)
{
    for (size_t i = 0; i < sizeof(MCP_TOOL_TABLE) / sizeof(MCP_TOOL_TABLE[0]); i++) {
        if (strcmp(MCP_TOOL_TABLE[i].name, name) == 0) return &MCP_TOOL_TABLE[i];
    }
    return NULL;
}

int mcp_registry_build_tools_list(cJSON *tools_array, cJSON *names_array)
{
    for (size_t i = 0; i < sizeof(MCP_TOOL_TABLE) / sizeof(MCP_TOOL_TABLE[0]); i++) {
        const mcp_tool_desc_t *desc = &MCP_TOOL_TABLE[i];
        cJSON *tool = cJSON_CreateObject();
        cJSON *schema = desc->input_schema();
        cJSON *annotations = cJSON_CreateObject();
        if (tool == NULL || schema == NULL || annotations == NULL) {
            cJSON_Delete(tool);
            cJSON_Delete(schema);
            cJSON_Delete(annotations);
            return -1;
        }
        cJSON_AddStringToObject(tool, "name", desc->name);
        cJSON_AddStringToObject(tool, "description", desc->description);
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddBoolToObject(annotations, "readOnlyHint", desc->read_only);
        cJSON_AddBoolToObject(annotations, "destructiveHint", desc->destructive);
        cJSON_AddBoolToObject(annotations, "idempotentHint", desc->read_only);
        cJSON_AddItemToObject(tool, "annotations", annotations);
        cJSON_AddItemToArray(tools_array, tool);

        cJSON *name_copy = cJSON_CreateString(desc->name);
        if (name_copy == NULL) return -1;
        cJSON_AddItemToArray(names_array, name_copy);
    }
    return 0;
}
