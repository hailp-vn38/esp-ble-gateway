#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "sdkconfig.h"

#include "mcp_endpoint_internal.h"
#include "mcp_tool_exposure.h"

// Strict tool registry: the only tools exposed over MCP. tools/list is built
// exclusively from this table, closing the hidden command surface that the
// old unknown-tool fallback allowed.

#ifndef CONFIG_MCP_KEEP_GENERIC_DEVICE_COMMAND
#define CONFIG_MCP_KEEP_GENERIC_DEVICE_COMMAND 0
#endif
#ifndef CONFIG_MCP_EXPOSE_FULL_CAPABILITY_TOOL
#define CONFIG_MCP_EXPOSE_FULL_CAPABILITY_TOOL 0
#endif

#define MAX_LEN_OF(field) ((int)(sizeof(((gw_message_t *)0)->field) - 1))

#define SCHEMA_FAIL(schema)       \
    do {                          \
        cJSON_Delete(schema);     \
        return NULL;              \
    } while (0)

#if CONFIG_MCP_EXPOSE_FULL_CAPABILITY_TOOL || CONFIG_MCP_KEEP_GENERIC_DEVICE_COMMAND
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
#endif

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

#define SCHEMA_FAIL(schema)       \
    do {                          \
        cJSON_Delete(schema);     \
        return NULL;              \
    } while (0)

static cJSON *schema_empty(void)
{
    return new_object_schema();
}

#if CONFIG_MCP_EXPOSE_FULL_CAPABILITY_TOOL
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
#endif

#if CONFIG_MCP_KEEP_GENERIC_DEVICE_COMMAND
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
#endif

#undef SCHEMA_FAIL

// Production static tools: get_status + list_devices always present.
// list_device_capabilities and device_command are debug/migration only.
static const mcp_tool_desc_t MCP_STATIC_TOOLS[] = {
    {"get_status", "Get gateway and BLE status", schema_empty, true, false},
    {"list_devices", "List devices known by the gateway", schema_empty, true, false},
#if CONFIG_MCP_EXPOSE_FULL_CAPABILITY_TOOL
    {"list_device_capabilities", "List commands advertised by a BLE device",
     schema_device_id, true, false},
#endif
#if CONFIG_MCP_KEEP_GENERIC_DEVICE_COMMAND
    {"device_command", "Send an allowlisted command to a device",
     schema_device_command, false, false},
#endif
};

const mcp_tool_desc_t *mcp_registry_find(const char *name)
{
    for (size_t i = 0; i < sizeof(MCP_STATIC_TOOLS) / sizeof(MCP_STATIC_TOOLS[0]); i++) {
        if (strcmp(MCP_STATIC_TOOLS[i].name, name) == 0) return &MCP_STATIC_TOOLS[i];
    }
    return NULL;
}

// Build static tools array only (§12.9).
int mcp_registry_build_tools_list(cJSON *tools_array)
{
    for (size_t i = 0; i < sizeof(MCP_STATIC_TOOLS) / sizeof(MCP_STATIC_TOOLS[0]); i++) {
        const mcp_tool_desc_t *desc = &MCP_STATIC_TOOLS[i];
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
        cJSON_AddStringToObject(tool, "title", desc->name);
        cJSON_AddStringToObject(tool, "description", desc->description);
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddBoolToObject(annotations, "readOnlyHint", desc->read_only);
        cJSON_AddBoolToObject(annotations, "destructiveHint", desc->destructive);
        if (desc->read_only) {
            cJSON_AddBoolToObject(annotations, "idempotentHint", true);
        }
        cJSON_AddStringToObject(annotations, "title", desc->name);
        cJSON_AddItemToObject(tool, "annotations", annotations);
        cJSON_AddItemToArray(tools_array, tool);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Dynamic tool schema builder from device capability
// ---------------------------------------------------------------------------

cJSON *mcp_dynamic_tool_build_schema(const device_capability_t *cap)
{
    cJSON *schema = new_object_schema();
    if (schema == NULL) return NULL;
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (properties == NULL) {
        cJSON_Delete(schema);
        return NULL;
    }

    if (cap->value_type == DEVICE_CAP_VALUE_NONE) {
        // No arguments needed — empty object.
        return schema;
    }

    cJSON *value_prop = cJSON_CreateObject();
    if (value_prop == NULL) {
        cJSON_Delete(schema);
        return NULL;
    }

    if (cap->value_type == DEVICE_CAP_VALUE_BOOL) {
        cJSON_AddStringToObject(value_prop, "type", "boolean");
    } else if (cap->value_type == DEVICE_CAP_VALUE_INT) {
        cJSON_AddStringToObject(value_prop, "type", "integer");
        cJSON_AddNumberToObject(value_prop, "minimum", cap->min_value);
        cJSON_AddNumberToObject(value_prop, "maximum", cap->max_value);
        if (cap->step > 0) {
            cJSON_AddNumberToObject(value_prop, "multipleOf", cap->step);
        }
        char desc_buf[64];
        if (cap->unit[0] != '\0') {
            snprintf(desc_buf, sizeof(desc_buf), "Value (%s)", cap->unit);
        } else {
            snprintf(desc_buf, sizeof(desc_buf), "Value");
        }
        cJSON_AddStringToObject(value_prop, "description", desc_buf);
    }

    cJSON_AddItemToObject(properties, "value", value_prop);

    cJSON *required = cJSON_CreateArray();
    if (required == NULL) {
        cJSON_Delete(schema);
        return NULL;
    }
    cJSON_AddItemToArray(required, cJSON_CreateString("value"));
    cJSON_AddItemToObject(schema, "required", required);

    return schema;
}

// Build a single dynamic tool cJSON object from a binding.
cJSON *mcp_dynamic_tool_build_json(const mcp_tool_binding_t *binding)
{
    cJSON *tool = cJSON_CreateObject();
    if (tool == NULL) return NULL;

    cJSON *schema = mcp_dynamic_tool_build_schema(&binding->capability);
    cJSON *annotations = cJSON_CreateObject();
    if (schema == NULL || annotations == NULL) {
        cJSON_Delete(tool);
        cJSON_Delete(schema);
        cJSON_Delete(annotations);
        return NULL;
    }

    cJSON_AddStringToObject(tool, "name", binding->tool_name);

    // Xiaozhi surfaces both fields in different views. Keep one identity for
    // name/title and never expose the internal device id as display metadata.
    device_entry_t entry = {0};
    const char *device_display_name = "configured device";
    if (device_store_get(binding->device_id, &entry) == DEVICE_STORE_OK &&
        entry.name[0] != '\0') {
        device_display_name = entry.name;
    }
    cJSON_AddStringToObject(tool, "title", binding->tool_name);

    // Trusted template description — no raw peripheral text (§19.1).
    char desc[192];
    snprintf(desc, sizeof(desc), "Execute command '%s' on device '%s'.",
             binding->command, device_display_name);
    cJSON_AddStringToObject(tool, "description", desc);
    cJSON_AddItemToObject(tool, "inputSchema", schema);

    // Annotations (§19.4).
    cJSON_AddBoolToObject(annotations, "readOnlyHint", false);
    bool destructive = (binding->capability.flags & DEVICE_CAP_FLAG_DESTRUCTIVE) != 0;
    bool idempotent = (binding->capability.flags & DEVICE_CAP_FLAG_IDEMPOTENT) != 0;
    // Older MCP clients read ToolAnnotations.title instead of Tool.title.
    cJSON_AddStringToObject(annotations, "title", binding->tool_name);
    cJSON_AddBoolToObject(annotations, "destructiveHint", destructive);
    cJSON_AddBoolToObject(annotations, "idempotentHint", idempotent);
    cJSON_AddItemToObject(tool, "annotations", annotations);

    return tool;
}
