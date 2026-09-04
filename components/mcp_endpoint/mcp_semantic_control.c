#include "mcp_semantic_control.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "device_schema.h"
#include "device_store.h"
#include "device_template.h"

/* ── Device resolution ─────────────────────────────────────────────── */

mcp_sem_resolve_status_t mcp_sem_resolve_device(const cJSON *arg,
                                                 char *out, size_t out_len)
{
    if (!cJSON_IsString(arg) || arg->valuestring == NULL ||
        arg->valuestring[0] == '\0')
        return MCP_SEM_INVALID;

    device_entry_t exact;
    if (device_store_get(arg->valuestring, &exact) == DEVICE_STORE_OK) {
        strlcpy(out, arg->valuestring, out_len);
        return MCP_SEM_OK;
    }

    device_entry_t entries[DEVICE_STORE_MAX_DEVICES];
    size_t count = 0;
    if (device_store_snapshot(entries, DEVICE_STORE_MAX_DEVICES, &count) !=
        DEVICE_STORE_OK)
        return MCP_SEM_NOT_FOUND;

    const char *match = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, arg->valuestring) != 0) continue;
        if (match != NULL) return MCP_SEM_AMBIGUOUS;
        match = entries[i].device_id;
    }
    if (match == NULL) return MCP_SEM_NOT_FOUND;
    strlcpy(out, match, out_len);
    return MCP_SEM_OK;
}

/* ── Feature resolution ────────────────────────────────────────────── */

mcp_sem_resolve_status_t mcp_sem_resolve_feature(
    const device_schema_snapshot_t *schema,
    const cJSON *arg,
    device_schema_feature_t *out)
{
    if (!cJSON_IsString(arg) || arg->valuestring == NULL ||
        arg->valuestring[0] == '\0')
        return MCP_SEM_INVALID;

    /* Exact feature_id match preferred */
    for (size_t i = 0; i < schema->feature_count; ++i) {
        if (strcmp(schema->features[i].feature_id, arg->valuestring) == 0) {
            *out = schema->features[i];
            return MCP_SEM_OK;
        }
    }

    /* Fallback: semantic name from device_template (unique only) */
    const device_schema_feature_t *match = NULL;
    for (size_t i = 0; i < schema->feature_count; ++i) {
        const device_template_t *tpl = device_template_resolve(
            schema->features[i].feature_type,
            schema->features[i].feature_schema_version);
        if (tpl == NULL ||
            strcmp(tpl->semantic_name, arg->valuestring) != 0)
            continue;
        if (match != NULL) return MCP_SEM_AMBIGUOUS;
        match = &schema->features[i];
    }
    if (match == NULL) return MCP_SEM_NOT_FOUND;
    *out = *match;
    return MCP_SEM_OK;
}

/* ── Feature serialization ─────────────────────────────────────────── */

bool mcp_sem_serialize_feature(cJSON *array,
                               const device_schema_snapshot_t *schema,
                               const device_schema_feature_t *feature)
{
    const device_template_t *tpl = device_template_resolve(
        feature->feature_type, feature->feature_schema_version);
    if (tpl == NULL) return true;

    cJSON *item = cJSON_CreateObject();
    if (item == NULL) return false;

    cJSON_AddStringToObject(item, "feature_id", feature->feature_id);
    cJSON_AddStringToObject(item, "semantic_name", tpl->semantic_name);
    cJSON_AddStringToObject(item, "type",
                            device_template_feature_name(feature->feature_type));
    cJSON_AddStringToObject(item, "property",
                            device_template_property_name(feature->property_id));

    device_template_value_type_t type =
        device_template_property_value_type(feature->property_id);
    cJSON_AddStringToObject(item, "value_type",
                            type == DEVICE_TEMPLATE_VALUE_BOOL ? "bool" :
                            type == DEVICE_TEMPLATE_VALUE_INT  ? "int"  : "none");

    cJSON_AddBoolToObject(item, "writable",
                          feature->writable_tool_index >= 0);

    if (type == DEVICE_TEMPLATE_VALUE_INT &&
        feature->writable_tool_index >= 0 &&
        (size_t)feature->writable_tool_index < schema->tool_count) {
        const device_schema_tool_t *tool =
            &schema->tools[feature->writable_tool_index];
        cJSON_AddNumberToObject(item, "minimum", tool->min_value);
        cJSON_AddNumberToObject(item, "maximum", tool->max_value);
        cJSON_AddNumberToObject(item, "step", tool->step);
    }

    cJSON_AddItemToArray(array, item);
    return true;
}
