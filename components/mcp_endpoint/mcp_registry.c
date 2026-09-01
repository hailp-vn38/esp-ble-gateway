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

#define MAX_LEN_OF(field) ((int)(sizeof(((gw_message_t *)0)->field) - 1))

#define SCHEMA_FAIL(schema)       \
    do {                          \
        cJSON_Delete(schema);     \
        return NULL;              \
    } while (0)

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

#undef SCHEMA_FAIL

// Production static tools: get_status + list_devices always present.
static const mcp_tool_desc_t MCP_STATIC_TOOLS[] = {
    {"get_status", "Get gateway and BLE status", schema_empty, true, false},
    {"list_devices", "List devices known by the gateway", schema_empty, true, false},
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

cJSON *mcp_dynamic_tool_build_schema(const device_schema_tool_t *cap)
{
    cJSON *schema = new_object_schema();
    if (schema == NULL) return NULL;
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (properties == NULL) {
        cJSON_Delete(schema);
        return NULL;
    }

    if (cap->value_type == 0 /* NONE */) {
        // No arguments needed — empty object.
        return schema;
    }

    cJSON *value_prop = cJSON_CreateObject();
    if (value_prop == NULL) {
        cJSON_Delete(schema);
        return NULL;
    }

    if (cap->value_type == 1 /* BOOL */) {
        cJSON_AddStringToObject(value_prop, "type", "boolean");
    } else if (cap->value_type == 2 /* INT */) {
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
    if (binding->feature_id[0] != '\0') {
        /* Semantic tool — describe by feature purpose. */
        snprintf(desc, sizeof(desc),
                 "Control the %s on %s.",
                 binding->capability.label[0] != '\0'
                     ? binding->capability.label
                     : "device",
                 device_display_name);
    } else {
        snprintf(desc, sizeof(desc), "Execute command '%s' on device '%s'.",
                 binding->command, device_display_name);
    }
    cJSON_AddStringToObject(tool, "description", desc);
    cJSON_AddItemToObject(tool, "inputSchema", schema);

    // Feature metadata (V4-09).
    if (binding->feature_id[0] != '\0') {
        cJSON_AddStringToObject(tool, "featureId", binding->feature_id);
    }

    // Annotations (§19.4).
    cJSON_AddBoolToObject(annotations, "readOnlyHint", false);
    bool destructive = (binding->capability.flags & DEVICE_SCHEMA_FLAG_DESTRUCTIVE) != 0;
    bool idempotent = (binding->capability.flags & DEVICE_SCHEMA_FLAG_IDEMPOTENT) != 0;
    // Older MCP clients read ToolAnnotations.title instead of Tool.title.
    cJSON_AddStringToObject(annotations, "title", binding->tool_name);
    cJSON_AddBoolToObject(annotations, "destructiveHint", destructive);
    cJSON_AddBoolToObject(annotations, "idempotentHint", idempotent);
    cJSON_AddItemToObject(tool, "annotations", annotations);

    return tool;
}
