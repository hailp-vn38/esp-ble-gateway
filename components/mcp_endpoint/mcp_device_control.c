#include "mcp_endpoint_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "device_command_service.h"
#include "device_schema.h"
#include "device_state.h"
#include "device_template.h"
#include "esp_log.h"
#include "mcp_tool_exposure.h"

static const char *TAG = "mcp_device_ctrl";

/* ── Operation types ─────────────────────────────────────────────────── */

typedef enum {
    MCP_DEVICE_OP_DESCRIBE = 0,
    MCP_DEVICE_OP_READ,
    MCP_DEVICE_OP_SET,
} mcp_device_operation_t;

/* ── Resolution helpers ──────────────────────────────────────────────── */

static esp_err_t resolve_device(const cJSON *device_arg,
                                 char *device_id_out, size_t out_len)
{
    if (device_arg == NULL || !cJSON_IsString(device_arg)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *device_id = cJSON_GetStringValue(device_arg);
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Try exact device id first */
    device_entry_t entry;
    if (device_store_get(device_id, &entry) == DEVICE_STORE_OK) {
        strlcpy(device_id_out, device_id, out_len);
        return ESP_OK;
    }

    /* Try configured name via snapshot scan */
    device_entry_t entries[DEVICE_STORE_MAX_DEVICES];
    size_t count = 0;
    if (device_store_snapshot(entries, DEVICE_STORE_MAX_DEVICES, &count) != DEVICE_STORE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, device_id) == 0) {
            strlcpy(device_id_out, entries[i].device_id, out_len);
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t resolve_feature(const char *device_id,
                                  const char *feature_arg,
                                  device_schema_feature_t *out_feature)
{
    if (feature_arg == NULL || feature_arg[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    device_schema_snapshot_t cap;
    if (device_schema_get(device_id, &cap) != ESP_OK || !cap.has_committed) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Try exact feature_id match */
    for (size_t i = 0; i < cap.feature_count; i++) {
        if (strcmp(cap.features[i].feature_id, feature_arg) == 0) {
            *out_feature = cap.features[i];
            return ESP_OK;
        }
    }

    /* Try semantic template name match */
    for (size_t i = 0; i < cap.feature_count; i++) {
        const device_template_t *tpl = device_template_resolve(
            cap.features[i].feature_type, cap.features[i].feature_schema_version);
        if (tpl != NULL && strcmp(tpl->semantic_name, feature_arg) == 0) {
            *out_feature = cap.features[i];
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static const device_schema_tool_t *find_writable_tool(
    const device_schema_snapshot_t *cap,
    const device_schema_feature_t *feature)
{
    if (feature->writable_tool_index < 0 ||
        (size_t)feature->writable_tool_index >= cap->tool_count) {
        return NULL;
    }
    return &cap->tools[feature->writable_tool_index];
}

/* ── Describe operation (local) ──────────────────────────────────────── */

static cJSON *handle_describe(const char *device_id, const char *feature_id)
{
    device_schema_feature_t feature;
    if (resolve_feature(device_id, feature_id, &feature) != ESP_OK) {
        return NULL;
    }

    const device_template_t *tpl = device_template_resolve(
        feature.feature_type, feature.feature_schema_version);

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) return NULL;

    cJSON_AddStringToObject(result, "device_id", device_id);
    cJSON_AddStringToObject(result, "feature_id", feature.feature_id);

    if (tpl != NULL) {
        cJSON_AddStringToObject(result, "type",
                                device_template_feature_name(feature.feature_type));
        cJSON_AddStringToObject(result, "semantic_name", tpl->semantic_name);
    }

    /* Add property info */
    const char *prop_name = device_template_property_name(feature.property_id);
    cJSON_AddStringToObject(result, "property", prop_name);

    device_template_value_type_t vtype =
        device_template_property_value_type(feature.property_id);
    cJSON_AddStringToObject(result, "value_type",
                            vtype == DEVICE_TEMPLATE_VALUE_BOOL ? "bool" :
                            vtype == DEVICE_TEMPLATE_VALUE_INT ? "int" : "none");

    return result;
}

/* ── Read operation (local cache) ────────────────────────────────────── */

static cJSON *handle_read(const char *device_id,
                           const device_schema_feature_t *feature)
{
    device_state_entry_t state;
    esp_err_t err = device_state_get(device_id, feature->feature_id,
                                      feature->property_id, &state);

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) return NULL;

    cJSON_AddStringToObject(result, "device_id", device_id);
    cJSON_AddStringToObject(result, "feature_id", feature->feature_id);

    if (err == ESP_OK && state.valid) {
        device_template_value_type_t vtype =
            device_template_property_value_type(feature->property_id);
        if (vtype == DEVICE_TEMPLATE_VALUE_BOOL) {
            cJSON_AddBoolToObject(result, "value", state.value_bool);
        } else if (vtype == DEVICE_TEMPLATE_VALUE_INT) {
            cJSON_AddNumberToObject(result, "value", state.value_int);
        }
        cJSON_AddBoolToObject(result, "available", true);
    } else {
        cJSON_AddNullToObject(result, "value");
        cJSON_AddStringToObject(result, "error", "state_not_available");
    }

    return result;
}

/* ── Set operation (device service) ──────────────────────────────────── */

typedef struct {
    mcp_responder_t responder;
    cJSON *id;
    mcp_request_context_t protocol;
} set_async_context_t;

static void set_completion(const device_command_result_t *result, void *arg)
{
    set_async_context_t *ctx = arg;

    bool ok = (result->status == DEVICE_CMD_STATUS_OK);
    cJSON *resp = cJSON_CreateObject();
    if (resp != NULL) {
        cJSON_AddBoolToObject(resp, "success", ok);
        cJSON_AddNumberToObject(resp, "status", (int)result->status);
        if (!ok) {
            const char *err_str = "internal_error";
            switch (result->status) {
            case DEVICE_CMD_STATUS_BUSY: err_str = "busy"; break;
            case DEVICE_CMD_STATUS_NOT_CONNECTED: err_str = "not_connected"; break;
            case DEVICE_CMD_STATUS_TIMEOUT: err_str = "timeout"; break;
            case DEVICE_CMD_STATUS_DEVICE_REJECTED: err_str = "device_rejected"; break;
            case DEVICE_CMD_STATUS_INVALID_ARGUMENT: err_str = "invalid_value"; break;
            case DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND: err_str = "unsupported_command"; break;
            default: break;
            }
            cJSON_AddStringToObject(resp, "error", err_str);
        }
    }

    /* Build MCP response */
    cJSON *envelope = cJSON_CreateObject();
    if (envelope != NULL) {
        cJSON_AddStringToObject(envelope, "jsonrpc", "2.0");
        cJSON *response_id = ctx->id != NULL ? cJSON_Duplicate(ctx->id, true)
                                              : cJSON_CreateNull();
        cJSON_AddItemToObject(envelope, "id", response_id);

        if (resp != NULL) {
            cJSON_AddItemToObject(envelope, "result", resp);
        } else {
            cJSON *error = cJSON_CreateObject();
            cJSON_AddNumberToObject(error, "code", -32603);
            cJSON_AddStringToObject(error, "message", "Out of memory");
            cJSON_AddItemToObject(envelope, "error", error);
        }

        char *json = cJSON_PrintUnformatted(envelope);
        cJSON_Delete(envelope);
        if (json != NULL && ctx->responder.send_json != NULL) {
            ctx->responder.send_json(ctx->responder.context, json,
                                      strlen(json), NULL);
            cJSON_free(json);
        }
    } else {
        cJSON_Delete(resp);
    }

    cJSON_Delete(ctx->id);
    if (ctx->responder.release != NULL) {
        ctx->responder.release(ctx->responder.context);
    }
    free(ctx);
}

/* ── Public entry point ──────────────────────────────────────────────── */

cJSON *mcp_device_control_execute(const cJSON *params,
                                   const mcp_request_context_t *ctx,
                                   mcp_rpc_error_t *error)
{
    if (params == NULL) {
        *error = (mcp_rpc_error_t){-32602, "Missing params"};
        return NULL;
    }

    /* Parse operation */
    const cJSON *op_arg = cJSON_GetObjectItemCaseSensitive(params, "operation");
    if (op_arg == NULL || !cJSON_IsString(op_arg)) {
        *error = (mcp_rpc_error_t){-32602, "Missing operation"};
        return NULL;
    }
    const char *op_str = cJSON_GetStringValue(op_arg);
    mcp_device_operation_t operation;
    if (strcmp(op_str, "describe") == 0) {
        operation = MCP_DEVICE_OP_DESCRIBE;
    } else if (strcmp(op_str, "read") == 0) {
        operation = MCP_DEVICE_OP_READ;
    } else if (strcmp(op_str, "set") == 0) {
        operation = MCP_DEVICE_OP_SET;
    } else {
        *error = (mcp_rpc_error_t){-32602, "Invalid operation"};
        return NULL;
    }

    /* Resolve device */
    const cJSON *device_arg = cJSON_GetObjectItemCaseSensitive(params, "device");
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    if (resolve_device(device_arg, device_id, sizeof(device_id)) != ESP_OK) {
        *error = (mcp_rpc_error_t){-32602, "Device not found"};
        return NULL;
    }

    /* Resolve feature */
    const cJSON *feature_arg = cJSON_GetObjectItemCaseSensitive(params, "feature");
    device_schema_feature_t feature;
    if (resolve_feature(device_id,
                        feature_arg ? cJSON_GetStringValue(feature_arg) : NULL,
                        &feature) != ESP_OK) {
        *error = (mcp_rpc_error_t){-32602, "Feature not found"};
        return NULL;
    }

    /* Execute by operation */
    switch (operation) {
    case MCP_DEVICE_OP_DESCRIBE:
        return handle_describe(device_id, feature.feature_id);

    case MCP_DEVICE_OP_READ:
        return handle_read(device_id, &feature);

    case MCP_DEVICE_OP_SET: {
        /* Check exposure policy */
        mcp_tool_exposure_t exposure;
        if (mcp_tool_exposure_get_feature(device_id, feature.feature_id,
                                           &exposure) == ESP_OK) {
            if (!exposure.control_enabled) {
                *error = (mcp_rpc_error_t){-32602, "Feature disabled"};
                return NULL;
            }
            if (exposure.state != MCP_EXPOSURE_ENABLED) {
                *error = (mcp_rpc_error_t){-32602, "Capability needs review"};
                return NULL;
            }
        }

        /* Get writable tool */
        device_schema_snapshot_t cap;
        if (device_schema_get(device_id, &cap) != ESP_OK || !cap.has_committed) {
            *error = (mcp_rpc_error_t){-32602, "Capabilities not ready"};
            return NULL;
        }
        const device_schema_tool_t *writable = find_writable_tool(&cap, &feature);
        if (writable == NULL) {
            *error = (mcp_rpc_error_t){-32602, "Feature is read-only"};
            return NULL;
        }

        /* Validate value */
        const cJSON *bool_val = cJSON_GetObjectItemCaseSensitive(params, "bool_value");
        const cJSON *int_val = cJSON_GetObjectItemCaseSensitive(params, "int_value");

        device_command_request_t request = {0};
        request.origin = DEVICE_CMD_ORIGIN_CONTROL;
        strlcpy(request.device_id, device_id, sizeof(request.device_id));
        strlcpy(request.command, writable->command, sizeof(request.command));

        device_template_value_type_t vtype =
            device_template_property_value_type(feature.property_id);

        if (vtype == DEVICE_TEMPLATE_VALUE_BOOL) {
            if (bool_val == NULL || !cJSON_IsBool(bool_val)) {
                *error = (mcp_rpc_error_t){-32602, "Missing bool_value"};
                return NULL;
            }
            request.bool_value = cJSON_IsTrue(bool_val);
            request.has_bool_value = true;
        } else if (vtype == DEVICE_TEMPLATE_VALUE_INT) {
            if (int_val == NULL || !cJSON_IsNumber(int_val)) {
                *error = (mcp_rpc_error_t){-32602, "Missing int_value"};
                return NULL;
            }
            int32_t val = int_val->valueint;
            if (val < writable->min_value || val > writable->max_value) {
                *error = (mcp_rpc_error_t){-32602, "Value out of range"};
                return NULL;
            }
            request.int_value = val;
            request.has_int_value = true;
        } else {
            *error = (mcp_rpc_error_t){-32602, "Unsupported property type"};
            return NULL;
        }

        /* Return NULL to indicate async - caller must use dispatch_device_control_async */
        *error = (mcp_rpc_error_t){0, NULL};
        /* Store request info in error data for caller */
        return (cJSON *)(intptr_t)-1; /* Sentinel: async path needed */
    }
    }

    *error = (mcp_rpc_error_t){-32603, "Internal error"};
    return NULL;
}

esp_err_t mcp_device_control_dispatch_async(
    const cJSON *params,
    const mcp_responder_t *responder,
    cJSON *id,
    const mcp_request_context_t *protocol)
{
    if (params == NULL || responder == NULL) return ESP_ERR_INVALID_ARG;

    /* Re-parse to build service request (params may be consumed) */
    const cJSON *device_arg = cJSON_GetObjectItemCaseSensitive(params, "device");
    const cJSON *feature_arg = cJSON_GetObjectItemCaseSensitive(params, "feature");
    const cJSON *op_arg = cJSON_GetObjectItemCaseSensitive(params, "operation");
    const cJSON *bool_val = cJSON_GetObjectItemCaseSensitive(params, "bool_value");
    const cJSON *int_val = cJSON_GetObjectItemCaseSensitive(params, "int_value");

    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    if (resolve_device(device_arg, device_id, sizeof(device_id)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    device_schema_feature_t feature;
    const char *feat_str = feature_arg ? cJSON_GetStringValue(feature_arg) : NULL;
    if (resolve_feature(device_id, feat_str, &feature) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    device_schema_snapshot_t cap;
    if (device_schema_get(device_id, &cap) != ESP_OK || !cap.has_committed) {
        return ESP_ERR_INVALID_STATE;
    }
    const device_schema_tool_t *writable = find_writable_tool(&cap, &feature);
    if (writable == NULL) return ESP_ERR_INVALID_STATE;

    /* Build service request */
    device_command_request_t request = {0};
    request.origin = DEVICE_CMD_ORIGIN_CONTROL;
    strlcpy(request.device_id, device_id, sizeof(request.device_id));
    strlcpy(request.command, writable->command, sizeof(request.command));

    device_template_value_type_t vtype =
        device_template_property_value_type(feature.property_id);
    if (vtype == DEVICE_TEMPLATE_VALUE_BOOL && bool_val != NULL) {
        request.bool_value = cJSON_IsTrue(bool_val);
        request.has_bool_value = true;
    } else if (vtype == DEVICE_TEMPLATE_VALUE_INT && int_val != NULL) {
        request.int_value = int_val->valueint;
        request.has_int_value = true;
    }

    /* Async context */
    set_async_context_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) return ESP_ERR_NO_MEM;
    ctx->responder = *responder;
    ctx->id = id != NULL ? cJSON_Duplicate(id, true) : NULL;
    ctx->protocol = *protocol;

    esp_err_t err = device_command_service_submit(&request, set_completion, ctx);
    if (err != ESP_OK) {
        cJSON_Delete(ctx->id);
        free(ctx);
    }
    return err;
}
