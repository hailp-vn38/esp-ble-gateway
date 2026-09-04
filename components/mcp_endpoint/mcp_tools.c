#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "device_schema.h"
#include "device_store.h"
#include "gateway_status.h"
#include "memory_policy.h"
#include "mcp_endpoint_internal.h"
#include "mcp_tool_exposure.h"

// ---------------------------------------------------------------------------
// tools/list — dual-era (§9, §11)
// ---------------------------------------------------------------------------

cJSON *mcp_tools_list(const mcp_request_context_t *ctx)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();
    if (result == NULL || tools == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(tools);
        return NULL;
    }

    // MCP 2026-07-28 ListToolsResult (§11)
    if (ctx->era == MCP_ERA_2026_07_28) {
        cJSON_AddStringToObject(result, "resultType", "complete");
        cJSON_AddNumberToObject(result, "ttlMs", MCP_TOOLS_CACHE_TTL_MS);
        cJSON_AddStringToObject(result, "cacheScope", MCP_TOOLS_CACHE_SCOPE);
    }

    // Static tools (includes device_control for compact).
    if (mcp_registry_build_tools_list(tools) != 0) {
        cJSON_Delete(result);
        cJSON_Delete(tools);
        return NULL;
    }

    cJSON_AddItemToObject(result, "tools", tools);

    // serverInfo in result._meta (§12.4)
    if (ctx->era == MCP_ERA_2026_07_28) {
        if (!mcp_result_add_server_info(result)) {
            cJSON_Delete(result);
            return NULL;
        }
    }

    return result;
}

mcp_resolve_status_t mcp_tools_resolve(const cJSON *params, gw_message_t *msg,
                                       mcp_tool_exec_kind_t *exec_kind,
                                       char *denial_text, size_t denial_len,
                                       mcp_rpc_error_t *error)
{
    memset(msg, 0, sizeof(*msg));
    *exec_kind = MCP_TOOL_EXEC_LOCAL;
    denial_text[0] = '\0';

    if (!cJSON_IsObject(params)) {
        *error = (mcp_rpc_error_t){-32602, "params must be an object"};
        return MCP_RESOLVE_INVALID;
    }
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (arguments != NULL && !cJSON_IsObject(arguments)) {
        *error = (mcp_rpc_error_t){-32602, "arguments must be an object"};
        return MCP_RESOLVE_INVALID;
    }
    const cJSON *source = cJSON_IsObject(arguments) ? arguments : params;
    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (name_item != NULL && !cJSON_IsString(name_item)) {
        *error = (mcp_rpc_error_t){-32602, "tool name must be a string"};
        return MCP_RESOLVE_INVALID;
    }
    const char *tool_name = cJSON_IsString(name_item)
                                ? name_item->valuestring
                                : cJSON_GetStringValue(
                                      cJSON_GetObjectItemCaseSensitive(
                                          source, "command"));
    if (tool_name == NULL || tool_name[0] == '\0') {
        *error = (mcp_rpc_error_t){-32602, "missing or invalid tool name"};
        return MCP_RESOLVE_INVALID;
    }

    // 1. Try static registry first.
    const mcp_tool_desc_t *desc = mcp_registry_find(tool_name);
    if (desc != NULL) {
        /* device_control is a compact semantic tool — route locally */
        if (strcmp(tool_name, "device_control") == 0) {
            *exec_kind = MCP_TOOL_EXEC_LOCAL;
            return MCP_RESOLVE_OK;
        }
        /* get_status / list_devices — direct typed API */
        if (strcmp(tool_name, "get_status") == 0 ||
            strcmp(tool_name, "list_devices") == 0) {
            *exec_kind = MCP_TOOL_EXEC_TYPED;
            return MCP_RESOLVE_OK;
        }
    }

    // 2. Unknown tool.
    *error = (mcp_rpc_error_t){-32602, "unknown tool"};
    return MCP_RESOLVE_INVALID;
}

// ---------------------------------------------------------------------------
// Tool error formatting — synthetic MCP error result envelope
// ---------------------------------------------------------------------------

cJSON *mcp_tools_tool_error(const char *text, const mcp_request_context_t *ctx,
                            mcp_rpc_error_t *err)
{
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        *err = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }

    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    if (content == NULL || item == NULL) {
        cJSON_Delete(content);
        cJSON_Delete(item);
        cJSON_Delete(out);
        *err = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(out, "content", content);
    cJSON_AddBoolToObject(out, "isError", true);

    if (ctx->era == MCP_ERA_2026_07_28) {
        cJSON_AddStringToObject(out, "resultType", "complete");
        if (!mcp_result_add_server_info(out)) {
            cJSON_Delete(out);
            *err = (mcp_rpc_error_t){-32603, "out of memory"};
            return NULL;
        }
    }

    return out;
}

cJSON *mcp_tools_format_device_result(const device_command_result_t *result,
                                      const mcp_request_context_t *ctx,
                                      mcp_rpc_error_t *err)
{
    bool ok = (result->status == DEVICE_CMD_STATUS_OK);

    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        *err = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }

    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    if (content == NULL || item == NULL) {
        cJSON_Delete(content);
        cJSON_Delete(item);
        cJSON_Delete(out);
        *err = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }
    cJSON_AddStringToObject(item, "type", "text");

    /* Format result as JSON text */
    cJSON *result_json = cJSON_CreateObject();
    if (result_json != NULL) {
        cJSON_AddBoolToObject(result_json, "success", ok);
        cJSON_AddNumberToObject(result_json, "status", (int)result->status);
        char *printed = cJSON_PrintUnformatted(result_json);
        cJSON_Delete(result_json);
        if (printed != NULL) {
            cJSON_AddStringToObject(item, "text", printed);
            cJSON_free(printed);
        } else {
            cJSON_AddStringToObject(item, "text", ok ? "{}" : "{\"error\":true}");
        }
    } else {
        cJSON_AddStringToObject(item, "text", ok ? "{}" : "{\"error\":true}");
    }

    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(out, "content", content);
    cJSON_AddBoolToObject(out, "isError", !ok);

    if (ctx->era == MCP_ERA_2026_07_28) {
        cJSON_AddStringToObject(out, "resultType", "complete");
        if (!mcp_result_add_server_info(out)) {
            cJSON_Delete(out);
            *err = (mcp_rpc_error_t){-32603, "out of memory"};
            return NULL;
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Typed local handlers — get_status / list_devices (Phase 7)
// ---------------------------------------------------------------------------

static void format_ble_addr(const uint8_t addr[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static cJSON *build_status_json(void)
{
    gateway_status_t st;
    if (gateway_status_get(&st) != ESP_OK) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;

    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "device_count", st.device_count);
    cJSON_AddNumberToObject(root, "connected_count", st.connected_count);
    cJSON_AddNumberToObject(root, "ble_link_count", st.ble_link_count);

    cJSON *internal = cJSON_AddObjectToObject(root, "internal");
    if (internal != NULL) {
        cJSON_AddNumberToObject(internal, "free", st.internal_free);
        cJSON_AddNumberToObject(internal, "min_free", st.internal_min_free);
        cJSON_AddNumberToObject(internal, "largest_free_block",
                                st.internal_largest_free_block);
    }

    cJSON *psram = cJSON_AddObjectToObject(root, "psram");
    if (psram != NULL) {
        cJSON_AddBoolToObject(psram, "ready", st.psram_ready);
        cJSON_AddNumberToObject(psram, "free", st.psram_free);
        cJSON_AddNumberToObject(psram, "min_free", st.psram_min_free);
        cJSON_AddNumberToObject(psram, "largest_free_block",
                                st.psram_largest_free_block);
    }

    cJSON *mem = cJSON_AddObjectToObject(root, "memory_policy");
    if (mem != NULL) {
        cJSON_AddNumberToObject(mem, "external_alloc_success",
                                st.memory_metrics.external_alloc_success);
        cJSON_AddNumberToObject(mem, "external_alloc_fail",
                                st.memory_metrics.external_alloc_fail);
        cJSON_AddNumberToObject(mem, "internal_fallback_attempt",
                                st.memory_metrics.internal_fallback_attempt);
        cJSON_AddNumberToObject(mem, "internal_fallback_success",
                                st.memory_metrics.internal_fallback_success);
        cJSON_AddNumberToObject(mem, "internal_fallback_rejected_floor",
                                st.memory_metrics.internal_fallback_rejected_floor);
    }

    return root;
}

cJSON *mcp_tools_execute_get_status(const mcp_request_context_t *ctx,
                                    mcp_rpc_error_t *error)
{
    cJSON *payload = build_status_json();
    if (payload == NULL) {
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }

    /* Wrap in MCP content envelope */
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        cJSON_Delete(payload);
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }

    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    if (content == NULL || item == NULL) {
        cJSON_Delete(content);
        cJSON_Delete(item);
        cJSON_Delete(out);
        cJSON_Delete(payload);
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }

    cJSON_AddStringToObject(item, "type", "text");
    char *printed = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (printed != NULL) {
        cJSON_AddStringToObject(item, "text", printed);
        cJSON_free(printed);
    } else {
        cJSON_AddStringToObject(item, "text", "{}");
    }
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(out, "content", content);
    cJSON_AddBoolToObject(out, "isError", false);

    if (ctx->era == MCP_ERA_2026_07_28) {
        cJSON_AddStringToObject(out, "resultType", "complete");
        if (!mcp_result_add_server_info(out)) {
            cJSON_Delete(out);
            *error = (mcp_rpc_error_t){-32603, "out of memory"};
            return NULL;
        }
    }

    /* structuredContent: parse the JSON payload into the envelope */
    if (printed != NULL) {
        /* printed was already freed; re-serialize from the root we built.
           Actually, payload was deleted. Rebuild from out's text content. */
    }
    /* We need to re-parse. Let's re-serialize from the content text. */
    cJSON *text_item = cJSON_GetArrayItem(
        cJSON_GetObjectItemCaseSensitive(out, "content"), 0);
    const char *text = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(text_item, "text"));
    if (text != NULL) {
        cJSON *structured = cJSON_Parse(text);
        if (structured != NULL) {
            if (ctx->era == MCP_ERA_2026_07_28) {
                cJSON_AddItemToObject(out, "structuredContent", structured);
            } else if (cJSON_IsObject(structured)) {
                cJSON_AddItemToObject(out, "structuredContent", structured);
            } else {
                cJSON_Delete(structured);
            }
        }
    }

    return out;
}

cJSON *mcp_tools_execute_list_devices(const mcp_request_context_t *ctx,
                                      mcp_rpc_error_t *error)
{
    device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
    size_t count = 0;
    if (device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES, &count) !=
        DEVICE_STORE_OK) {
        *error = (mcp_rpc_error_t){-32603, "could not read device list"};
        return NULL;
    }

    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) continue;

        cJSON_AddStringToObject(item, "device_id", devices[i].device_id);
        cJSON_AddStringToObject(item, "name", devices[i].name);

        /* Schema status */
        device_schema_snapshot_t schema = {0};
        bool schema_available =
            device_schema_get(devices[i].device_id, &schema) == ESP_OK &&
            schema.has_committed;
        size_t writable_count = 0;
        if (schema_available) {
            for (size_t f = 0; f < schema.feature_count; ++f) {
                if (schema.features[f].writable_tool_index >= 0 &&
                    (size_t)schema.features[f].writable_tool_index <
                        schema.tool_count) {
                    ++writable_count;
                }
            }
        }

        cJSON *capabilities = cJSON_AddObjectToObject(item, "capabilities");
        if (capabilities != NULL) {
            cJSON_AddBoolToObject(capabilities, "available", schema_available);
            cJSON_AddStringToObject(capabilities, "state",
                                    device_schema_state_name(schema.state));
            cJSON_AddNumberToObject(capabilities, "feature_count",
                                    schema_available ? schema.feature_count : 0);
            cJSON_AddNumberToObject(capabilities, "writable_feature_count",
                                    schema_available ? writable_count : 0);
            if (schema_available) {
                cJSON_AddNumberToObject(capabilities, "revision",
                                        schema.revision);
            }
        }

        /* Control hints via shared semantic helper */
        cJSON *controls = cJSON_AddArrayToObject(item, "controls");
        mcp_control_hint_t hints[MCP_SEMANTIC_CONTROL_HINT_MAX] = {0};
        size_t hint_count = 0;
        bool controls_truncated = false;
        if (controls != NULL &&
            mcp_semantic_control_get_hints(
                devices[i].device_id, hints, MCP_SEMANTIC_CONTROL_HINT_MAX,
                &hint_count, &controls_truncated) == ESP_OK) {
            if (mcp_semantic_control_serialize_hints(controls, hints,
                                                    hint_count) != ESP_OK)
                controls_truncated = true;
        }
        if (controls_truncated)
            cJSON_AddBoolToObject(item, "controls_truncated", true);

        /* BLE address */
        cJSON_AddBoolToObject(item, "has_ble_addr", devices[i].has_ble_identity);
        if (devices[i].has_ble_identity) {
            char address[18];
            format_ble_addr(devices[i].ble_addr, address);
            cJSON_AddStringToObject(item, "ble_addr", address);
            cJSON_AddNumberToObject(item, "ble_addr_type",
                                    devices[i].ble_addr_type);
        }

        cJSON_AddItemToArray(array, item);
    }

    /* Truncate if payload too large for MCP content envelope */
    char *printed = cJSON_PrintUnformatted(array);
    if (printed == NULL) {
        /* Try removing controls one by one from the last device */
        bool trimmed = false;
        for (int i = cJSON_GetArraySize(array) - 1;
             i >= 0 && !trimmed; --i) {
            cJSON *dev = cJSON_GetArrayItem(array, i);
            cJSON *ctl = cJSON_GetObjectItemCaseSensitive(dev, "controls");
            int ctl_count = cJSON_GetArraySize(ctl);
            if (ctl_count <= 0) continue;
            cJSON_DeleteItemFromArray(ctl, ctl_count - 1);
            cJSON *trunc = cJSON_GetObjectItemCaseSensitive(
                dev, "controls_truncated");
            if (trunc != NULL)
                cJSON_SetBoolValue(trunc, true);
            else
                cJSON_AddBoolToObject(dev, "controls_truncated", true);
            trimmed = true;
        }
        if (trimmed)
            printed = cJSON_PrintUnformatted(array);
    }
    cJSON_Delete(array);

    if (printed == NULL) {
        *error = (mcp_rpc_error_t){-32603, "device list too large"};
        return NULL;
    }

    /* Wrap in MCP content envelope */
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        cJSON_free(printed);
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }

    cJSON *content = cJSON_CreateArray();
    cJSON *content_item = cJSON_CreateObject();
    if (content == NULL || content_item == NULL) {
        cJSON_Delete(content);
        cJSON_Delete(content_item);
        cJSON_Delete(out);
        cJSON_free(printed);
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }
    cJSON_AddStringToObject(content_item, "type", "text");
    cJSON_AddStringToObject(content_item, "text", printed);
    cJSON_free(printed);
    cJSON_AddItemToArray(content, content_item);
    cJSON_AddItemToObject(out, "content", content);
    cJSON_AddBoolToObject(out, "isError", false);

    if (ctx->era == MCP_ERA_2026_07_28) {
        cJSON_AddStringToObject(out, "resultType", "complete");
        if (!mcp_result_add_server_info(out)) {
            cJSON_Delete(out);
            *error = (mcp_rpc_error_t){-32603, "out of memory"};
            return NULL;
        }
    }

    /* structuredContent: re-parse the JSON text */
    cJSON *structured = cJSON_Parse(
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content_item, "text")));
    if (structured != NULL) {
        if (ctx->era == MCP_ERA_2026_07_28) {
            cJSON_AddItemToObject(out, "structuredContent", structured);
        } else if (cJSON_IsObject(structured)) {
            cJSON_AddItemToObject(out, "structuredContent", structured);
        } else {
            cJSON_Delete(structured);
        }
    }

    return out;
}
