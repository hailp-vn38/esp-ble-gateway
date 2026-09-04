#include "mcp_endpoint_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "device_schema.h"
#include "device_state.h"
#include "device_template.h"

typedef enum { OP_DESCRIBE = 0, OP_READ, OP_SET } operation_t;
typedef enum { RESOLVE_OK = 0, RESOLVE_NOT_FOUND, RESOLVE_AMBIGUOUS,
               RESOLVE_INVALID } resolve_status_t;

static resolve_status_t resolve_device(const cJSON *arg, char *out,
                                       size_t out_len)
{
    if (!cJSON_IsString(arg) || arg->valuestring == NULL ||
        arg->valuestring[0] == '\0') return RESOLVE_INVALID;
    device_entry_t exact;
    if (device_store_get(arg->valuestring, &exact) == DEVICE_STORE_OK) {
        strlcpy(out, arg->valuestring, out_len);
        return RESOLVE_OK;
    }
    device_entry_t entries[DEVICE_STORE_MAX_DEVICES];
    size_t count = 0;
    if (device_store_snapshot(entries, DEVICE_STORE_MAX_DEVICES, &count) !=
        DEVICE_STORE_OK) return RESOLVE_NOT_FOUND;
    const char *match = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, arg->valuestring) != 0) continue;
        if (match != NULL) return RESOLVE_AMBIGUOUS;
        match = entries[i].device_id;
    }
    if (match == NULL) return RESOLVE_NOT_FOUND;
    strlcpy(out, match, out_len);
    return RESOLVE_OK;
}

static resolve_status_t resolve_feature(const device_schema_snapshot_t *schema,
                                        const cJSON *arg,
                                        device_schema_feature_t *out)
{
    if (!cJSON_IsString(arg) || arg->valuestring == NULL ||
        arg->valuestring[0] == '\0') return RESOLVE_INVALID;
    for (size_t i = 0; i < schema->feature_count; ++i) {
        if (strcmp(schema->features[i].feature_id, arg->valuestring) == 0) {
            *out = schema->features[i];
            return RESOLVE_OK;
        }
    }
    const device_schema_feature_t *match = NULL;
    for (size_t i = 0; i < schema->feature_count; ++i) {
        const device_template_t *tpl = device_template_resolve(
            schema->features[i].feature_type,
            schema->features[i].feature_schema_version);
        if (tpl == NULL || strcmp(tpl->semantic_name, arg->valuestring) != 0)
            continue;
        if (match != NULL) return RESOLVE_AMBIGUOUS;
        match = &schema->features[i];
    }
    if (match == NULL) return RESOLVE_NOT_FOUND;
    *out = *match;
    return RESOLVE_OK;
}

static bool add_feature(cJSON *array, const device_schema_snapshot_t *schema,
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
                            type == DEVICE_TEMPLATE_VALUE_INT ? "int" : "none");
    cJSON_AddBoolToObject(item, "writable", feature->writable_tool_index >= 0);
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

static cJSON *describe(const char *device_id,
                       const device_schema_snapshot_t *schema,
                       const device_schema_feature_t *filter)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *features = cJSON_CreateArray();
    if (payload == NULL || features == NULL) goto fail;
    cJSON_AddStringToObject(payload, "device_id", device_id);
    device_entry_t device = {0};
    if (device_store_get(device_id, &device) == DEVICE_STORE_OK) {
        cJSON_AddStringToObject(payload, "name", device.name);
    }
    cJSON_AddItemToObject(payload, "features", features);
    features = NULL;
    if (filter != NULL) {
        if (!add_feature(cJSON_GetObjectItem(payload, "features"), schema,
                         filter))
            goto fail;
    } else {
        for (size_t i = 0; i < schema->feature_count; ++i) {
            if (!add_feature(cJSON_GetObjectItem(payload, "features"), schema,
                             &schema->features[i])) goto fail;
        }
    }
    return payload;
fail:
    cJSON_Delete(features);
    cJSON_Delete(payload);
    return NULL;
}

static cJSON *read_cached(const char *device_id,
                          const device_schema_feature_t *feature,
                          bool *is_error)
{
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) return NULL;
    cJSON_AddStringToObject(payload, "device_id", device_id);
    cJSON_AddStringToObject(payload, "feature_id", feature->feature_id);
    device_template_value_type_t type =
        device_template_property_value_type(feature->property_id);
    if (type != DEVICE_TEMPLATE_VALUE_BOOL && type != DEVICE_TEMPLATE_VALUE_INT) {
        cJSON_AddStringToObject(payload, "error", "unsupported_property");
        *is_error = true;
        return payload;
    }
    device_state_entry_t state;
    if (device_state_get(device_id, feature->feature_id, feature->property_id,
                         &state) != ESP_OK || !state.valid) {
        cJSON_AddStringToObject(payload, "error", "state_not_available");
        *is_error = true;
        return payload;
    }
    if (type == DEVICE_TEMPLATE_VALUE_BOOL)
        cJSON_AddBoolToObject(payload, "value", state.value_bool);
    else
        cJSON_AddNumberToObject(payload, "value", state.value_int);
    return payload;
}

cJSON *mcp_device_control_format_result(const cJSON *payload, bool is_error,
                                         const mcp_request_context_t *protocol,
                                         mcp_rpc_error_t *error)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON *structured = payload != NULL ? cJSON_Duplicate(payload, true) : NULL;
    char *text = payload != NULL ? cJSON_PrintUnformatted(payload) : NULL;
    if (out == NULL || content == NULL || item == NULL || structured == NULL ||
        text == NULL) goto fail;
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_free(text);
    text = NULL;
    cJSON_AddItemToArray(content, item);
    item = NULL;
    cJSON_AddItemToObject(out, "content", content);
    content = NULL;
    cJSON_AddBoolToObject(out, "isError", is_error);
    cJSON_AddItemToObject(out, "structuredContent", structured);
    structured = NULL;
    if (protocol->era == MCP_ERA_2026_07_28) {
        cJSON_AddStringToObject(out, "resultType", "complete");
        if (!mcp_result_add_server_info(out)) goto fail;
    }
    return out;
fail:
    cJSON_free(text);
    cJSON_Delete(structured);
    cJSON_Delete(item);
    cJSON_Delete(content);
    cJSON_Delete(out);
    *error = (mcp_rpc_error_t){-32603, "out of memory"};
    return NULL;
}

static cJSON *tool_error(const char *code,
                         const mcp_request_context_t *protocol,
                         mcp_rpc_error_t *error)
{
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }
    cJSON_AddStringToObject(payload, "error", code);
    cJSON *result = mcp_device_control_format_result(payload, true, protocol,
                                                      error);
    cJSON_Delete(payload);
    return result;
}

cJSON *mcp_device_control_format_completion(
    const char *device_id, const char *feature_id,
    const device_command_result_t *result,
    const mcp_request_context_t *protocol, mcp_rpc_error_t *error)
{
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) return NULL;
    cJSON_AddStringToObject(payload, "device_id", device_id);
    cJSON_AddStringToObject(payload, "feature_id", feature_id);
    bool ok = result->status == DEVICE_CMD_STATUS_OK;
    cJSON_AddBoolToObject(payload, "success", ok);
    if (!ok) {
        const char *name = "internal_error";
        switch (result->status) {
        case DEVICE_CMD_STATUS_INVALID_ARGUMENT: name = "invalid_value"; break;
        case DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND: name = "unsupported_command"; break;
        case DEVICE_CMD_STATUS_BUSY: name = "busy"; break;
        case DEVICE_CMD_STATUS_NOT_CONNECTED: name = "not_connected"; break;
        case DEVICE_CMD_STATUS_TRANSPORT_ERROR: name = "transport_error"; break;
        case DEVICE_CMD_STATUS_TIMEOUT: name = "timeout"; break;
        case DEVICE_CMD_STATUS_DEVICE_REJECTED: name = "device_rejected"; break;
        default: break;
        }
        cJSON_AddStringToObject(payload, "error", name);
    }
    cJSON *formatted = mcp_device_control_format_result(payload, !ok, protocol,
                                                         error);
    cJSON_Delete(payload);
    return formatted;
}

static void set_tool_error(mcp_device_control_plan_t *plan, const char *code,
                           const mcp_request_context_t *protocol)
{
    plan->kind = MCP_DEVICE_CONTROL_EXEC_LOCAL;
    plan->local_result = tool_error(code, protocol, &plan->error);
    if (plan->local_result == NULL) plan->kind = MCP_DEVICE_CONTROL_EXEC_ERROR;
}

esp_err_t mcp_device_control_resolve(const cJSON *params,
                                     const mcp_request_context_t *protocol,
                                     mcp_device_control_plan_t *out)
{
    if (out == NULL || protocol == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->kind = MCP_DEVICE_CONTROL_EXEC_ERROR;
    if (!cJSON_IsObject(params)) {
        out->error = (mcp_rpc_error_t){-32602, "params must be an object"};
        return ESP_OK;
    }
    const cJSON *op_arg = cJSON_GetObjectItemCaseSensitive(params, "operation");
    const char *op = cJSON_IsString(op_arg) ? op_arg->valuestring : NULL;
    operation_t operation;
    if (op == NULL) {
        out->error = (mcp_rpc_error_t){-32602, "missing operation"}; return ESP_OK;
    } else if (strcmp(op, "describe") == 0) operation = OP_DESCRIBE;
    else if (strcmp(op, "read") == 0) operation = OP_READ;
    else if (strcmp(op, "set") == 0) operation = OP_SET;
    else {
        out->error = (mcp_rpc_error_t){-32602, "invalid operation"}; return ESP_OK;
    }

    resolve_status_t device_status = resolve_device(
        cJSON_GetObjectItemCaseSensitive(params, "device"), out->device_id,
        sizeof(out->device_id));
    if (device_status == RESOLVE_AMBIGUOUS)
        set_tool_error(out, "ambiguous_device", protocol);
    else if (device_status != RESOLVE_OK)
        set_tool_error(out, "device_not_found", protocol);
    if (out->kind != MCP_DEVICE_CONTROL_EXEC_ERROR) return ESP_OK;

    device_schema_snapshot_t schema;
    if (device_schema_get(out->device_id, &schema) != ESP_OK ||
        !schema.has_committed) {
        set_tool_error(out, "capabilities_not_ready", protocol); return ESP_OK;
    }
    const cJSON *feature_arg = cJSON_GetObjectItemCaseSensitive(params, "feature");
    device_schema_feature_t feature = {0};
    bool has_feature = feature_arg != NULL;
    if (operation != OP_DESCRIBE && !has_feature) {
        out->error = (mcp_rpc_error_t){-32602, "feature is required"}; return ESP_OK;
    }
    if (has_feature) {
        resolve_status_t status = resolve_feature(&schema, feature_arg, &feature);
        if (status == RESOLVE_AMBIGUOUS)
            set_tool_error(out, "ambiguous_feature", protocol);
        else if (status != RESOLVE_OK)
            set_tool_error(out, "feature_not_found", protocol);
        if (out->kind != MCP_DEVICE_CONTROL_EXEC_ERROR) return ESP_OK;
        strlcpy(out->feature_id, feature.feature_id, sizeof(out->feature_id));
    }

    if (operation == OP_DESCRIBE) {
        cJSON *payload = describe(out->device_id, &schema,
                                  has_feature ? &feature : NULL);
        if (payload == NULL) goto oom;
        out->local_result = mcp_device_control_format_result(
            payload, false, protocol, &out->error);
        cJSON_Delete(payload);
        out->kind = out->local_result != NULL ? MCP_DEVICE_CONTROL_EXEC_LOCAL
                                               : MCP_DEVICE_CONTROL_EXEC_ERROR;
        return ESP_OK;
    }
    if (operation == OP_READ) {
        bool is_error = false;
        cJSON *payload = read_cached(out->device_id, &feature, &is_error);
        if (payload == NULL) goto oom;
        out->local_result = mcp_device_control_format_result(
            payload, is_error, protocol, &out->error);
        cJSON_Delete(payload);
        out->kind = out->local_result != NULL ? MCP_DEVICE_CONTROL_EXEC_LOCAL
                                               : MCP_DEVICE_CONTROL_EXEC_ERROR;
        return ESP_OK;
    }

    if (feature.writable_tool_index < 0 ||
        (size_t)feature.writable_tool_index >= schema.tool_count) {
        set_tool_error(out, "feature_read_only", protocol); return ESP_OK;
    }
    const device_schema_tool_t *tool = &schema.tools[feature.writable_tool_index];
    mcp_policy_result_t policy = mcp_policy_check_feature_control(
        out->device_id, feature.feature_id, tool);
    if (policy != MCP_POLICY_ALLOW) {
        set_tool_error(out, policy == MCP_POLICY_DENY_DESTRUCTIVE
                                ? "destructive_denied" : "control_denied",
                       protocol);
        return ESP_OK;
    }
    const cJSON *bool_value = cJSON_GetObjectItemCaseSensitive(params, "bool_value");
    const cJSON *int_value = cJSON_GetObjectItemCaseSensitive(params, "int_value");
    if ((bool_value != NULL) == (int_value != NULL)) {
        out->error = (mcp_rpc_error_t){-32602, "exactly one typed value is required"};
        return ESP_OK;
    }
    device_template_value_type_t type =
        device_template_property_value_type(feature.property_id);
    if (type == DEVICE_TEMPLATE_VALUE_BOOL) {
        if (!cJSON_IsBool(bool_value)) {
            out->error = (mcp_rpc_error_t){-32602, "bool_value is required"};
            return ESP_OK;
        }
        out->request.has_bool_value = true;
        out->request.bool_value = cJSON_IsTrue(bool_value);
    } else if (type == DEVICE_TEMPLATE_VALUE_INT) {
        if (!cJSON_IsNumber(int_value) ||
            int_value->valuedouble != (double)int_value->valueint) {
            out->error = (mcp_rpc_error_t){-32602, "int_value must be an integer"};
            return ESP_OK;
        }
        out->request.has_int_value = true;
        out->request.int_value = int_value->valueint;
    } else {
        set_tool_error(out, "unsupported_property", protocol); return ESP_OK;
    }
    out->request.origin = DEVICE_CMD_ORIGIN_CONTROL;
    strlcpy(out->request.device_id, out->device_id, sizeof(out->request.device_id));
    strlcpy(out->request.command, tool->command, sizeof(out->request.command));
    strlcpy(out->request.feature_id, feature.feature_id,
            sizeof(out->request.feature_id));
    out->request.has_feature_id = true;
    out->request.property_id = feature.property_id;
    out->request.has_property_id = true;
    out->kind = MCP_DEVICE_CONTROL_EXEC_ASYNC_SET;
    return ESP_OK;
oom:
    out->error = (mcp_rpc_error_t){-32603, "out of memory"};
    out->kind = MCP_DEVICE_CONTROL_EXEC_ERROR;
    return ESP_OK;
}
