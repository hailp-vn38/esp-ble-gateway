#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "sdkconfig.h"

#include "mcp_endpoint_internal.h"

// Strict tool registry: the only tools exposed over MCP. tools/list is built
// exclusively from this table, closing the hidden command surface that the
// old unknown-tool fallback allowed.
//
// Admin tools (add_device, edit_device, delete_device) are NOT exposed
// through the MCP control/voice profile per spec §12. They remain
// available through Web UI / REST admin only.

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
    cJSON_AddBoolToObject(schema, "additionalProperties", false);
    return schema;
}

// Every partial-failure path frees the whole tree; cJSON_Delete(NULL) is a
// no-op so each helper can bail with {cJSON_Delete(root); return NULL;}
#define SCHEMA_FAIL(schema)       \
    do {                          \
        cJSON_Delete(schema);     \
        return NULL;              \
    } while (0)

static cJSON *schema_empty(void)
{
    return new_object_schema();
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

    // Add minLength constraints per spec §13.4
    cJSON *device_id_schema =
        cJSON_GetObjectItemCaseSensitive(properties, "device_id");
    if (device_id_schema != NULL) {
        cJSON_AddNumberToObject(device_id_schema, "minLength", 1);
    }
    cJSON *command_schema =
        cJSON_GetObjectItemCaseSensitive(properties, "command");
    if (command_schema != NULL) {
        cJSON_AddNumberToObject(command_schema, "minLength", 1);
    }

    return schema;
}

#undef SCHEMA_FAIL

// MCP control profile tools only — admin CRUD is excluded per spec §12.
// Deterministic order for stable caching and prompt behavior (spec §37).
static const mcp_tool_desc_t MCP_TOOL_TABLE[] = {
    {"get_status", "Get gateway and BLE status", schema_empty, true, false},
    {"list_devices", "List devices known by the gateway", schema_empty, true, false},
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
        // idempotentHint: only true for read-only tools; device_command
        // is a generic tool and cannot guarantee idempotency for all
        // dynamic commands, so omit it for non-read-only tools (spec §26).
        if (desc->read_only) {
            cJSON_AddBoolToObject(annotations, "idempotentHint", true);
        }
        cJSON_AddItemToObject(tool, "annotations", annotations);
        cJSON_AddItemToArray(tools_array, tool);

        cJSON *name_copy = cJSON_CreateString(desc->name);
        if (name_copy == NULL) return -1;
        cJSON_AddItemToArray(names_array, name_copy);
    }
    return 0;
}
