#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "cbor_codec.h"
#include "command_dispatcher.h"
#include "device_capabilities.h"
#include "memory_policy.h"
#include "mcp_endpoint_internal.h"
#include "mcp_tool_exposure.h"

// One static dispatch result guarded by a mutex: no per-request 4KB heap
// churn, and the same lock serializes dispatcher access between the HTTPD
// task (sync tools) and the async worker (device_command).

static SemaphoreHandle_t s_dispatch_mutex;
static dispatch_result_t s_dispatch_result;
static portMUX_TYPE s_mutex_guard = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t ensure_dispatch_mutex(void)
{
    SemaphoreHandle_t existing;
    taskENTER_CRITICAL(&s_mutex_guard);
    existing = s_dispatch_mutex;
    taskEXIT_CRITICAL(&s_mutex_guard);
    if (existing != NULL) return existing;

    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) return NULL;
    taskENTER_CRITICAL(&s_mutex_guard);
    if (s_dispatch_mutex != NULL) {
        SemaphoreHandle_t winner = s_dispatch_mutex;
        taskEXIT_CRITICAL(&s_mutex_guard);
        vSemaphoreDelete(created);
        return winner;
    }
    s_dispatch_mutex = created;
    taskEXIT_CRITICAL(&s_mutex_guard);
    return created;
}

// ---------------------------------------------------------------------------
// Command allowlist for the device_command tool
// ---------------------------------------------------------------------------

bool mcp_device_command_allowed(const char *command)
{
    const char *entry = CONFIG_MCP_DEVICE_COMMAND_ALLOWLIST;
    size_t command_len = strlen(command);
    while (*entry != '\0') {
        const char *comma = strchr(entry, ',');
        size_t entry_len = comma != NULL ? (size_t)(comma - entry)
                                         : strlen(entry);
        while (entry_len > 0 && *entry == ' ') {
            entry++;
            entry_len--;
        }
        if (entry_len == command_len &&
            strncmp(entry, command, command_len) == 0) {
            return true;
        }
        if (comma == NULL) break;
        entry = comma + 1;
    }
    return false;
}

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

    // Static tools first.
    if (mcp_registry_build_tools_list(tools) != 0) {
        cJSON_Delete(result);
        cJSON_Delete(tools);
        return NULL;
    }

    // Dynamic device tools (ENABLED only, deterministic order).  A binding is
    // large enough that the maximum snapshot does not fit on the 8 KiB MCP WS
    // task stack, so keep this bounded transient workspace off the stack.
    const size_t binding_capacity = CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED;
    mcp_tool_binding_t *bindings = gw_mem_alloc(
        binding_capacity * sizeof(*bindings), GW_MEM_EXTERNAL_PREFERRED);
    if (bindings == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(tools);
        return NULL;
    }

    size_t binding_count = 0;
    mcp_tool_catalog_get_snapshot(bindings, binding_capacity, &binding_count);
    for (size_t i = 0; i < binding_count; i++) {
        cJSON *tool = mcp_dynamic_tool_build_json(&bindings[i]);
        if (tool != NULL) {
            cJSON_AddItemToArray(tools, tool);
        }
    }
    gw_mem_free(bindings);

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

// ---------------------------------------------------------------------------
// params -> gw_message_t normalization
// ---------------------------------------------------------------------------

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

static mcp_resolve_status_t normalize_arguments(const cJSON *arguments,
                                                const char *message_type,
                                                const char *command,
                                                gw_message_t *msg,
                                                mcp_rpc_error_t *error)
{
    cJSON *normalized = cJSON_CreateObject();
    if (normalized == NULL) {
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return MCP_RESOLVE_INVALID;
    }
    cJSON_AddStringToObject(normalized, "type", message_type);
    cJSON_AddStringToObject(normalized, "command", command);
    const char *optional_fields[] = {
        "protocol_version", "device_id", "int_value", "bool_value", "name",
        "device_type", "ble_addr", "ble_addr_type"
    };
    bool copied = true;
    for (size_t i = 0; i < sizeof(optional_fields) / sizeof(optional_fields[0]); i++) {
        copied = copied && copy_optional_field(normalized, arguments,
                                               optional_fields[i]);
    }
    if (strcmp(message_type, "device_command") == 0 &&
        cJSON_GetObjectItemCaseSensitive(normalized, "int_value") == NULL) {
        const cJSON *legacy_value =
            cJSON_GetObjectItemCaseSensitive(arguments, "value");
        if (legacy_value != NULL) {
            cJSON *copy = cJSON_Duplicate(legacy_value, true);
            if (copy == NULL) {
                copied = false;
            } else {
                cJSON_AddItemToObject(normalized, "int_value", copy);
            }
        }
    }

    char *json = copied ? cJSON_PrintUnformatted(normalized) : NULL;
    cJSON_Delete(normalized);
    if (json == NULL) {
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return MCP_RESOLVE_INVALID;
    }
    int result = cbor_codec_json_to_msg(json, msg);
    cJSON_free(json);
    if (result != 0) {
        *error = (mcp_rpc_error_t){-32602, "invalid tool arguments"};
        return MCP_RESOLVE_INVALID;
    }
    return MCP_RESOLVE_OK;
}

mcp_resolve_status_t mcp_tools_resolve(const cJSON *params, gw_message_t *msg,
                                       bool *is_device_command,
                                       char *denial_text, size_t denial_len,
                                       mcp_rpc_error_t *error)
{
    memset(msg, 0, sizeof(*msg));
    *is_device_command = false;
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
        const char *message_type =
            strcmp(tool_name, "device_command") == 0 ? "device_command"
                                                     : "gateway_command";
        if (strcmp(message_type, "device_command") == 0) {
            const cJSON *device_id =
                cJSON_GetObjectItemCaseSensitive(source, "device_id");
            if (!cJSON_IsString(device_id) || device_id->valuestring == NULL ||
                device_id->valuestring[0] == '\0') {
                *error = (mcp_rpc_error_t){-32602, "device_command requires device_id"};
                return MCP_RESOLVE_INVALID;
            }
            const cJSON *command_item =
                cJSON_GetObjectItemCaseSensitive(source, "command");
            const char *device_command =
                cJSON_IsString(command_item) && command_item->valuestring != NULL &&
                        command_item->valuestring[0] != '\0'
                    ? command_item->valuestring
                    : NULL;
            if (device_command == NULL) {
                *error = (mcp_rpc_error_t){-32602,
                                           "device_command requires command"};
                return MCP_RESOLVE_INVALID;
            }
            if (!mcp_device_command_allowed(device_command)) {
                snprintf(denial_text, denial_len,
                         "command '%s' is not in the device command allowlist",
                         device_command);
                *error = (mcp_rpc_error_t){MCP_ERR_COMMAND_DENIED, "command denied"};
                return MCP_RESOLVE_ALLOWLIST_DENIED;
            }

            mcp_policy_result_t policy = mcp_policy_check_device_command(
                device_id->valuestring, device_command);
            if (policy == MCP_POLICY_DENY_COMMAND) {
                snprintf(denial_text, denial_len,
                         "command '%s' is not allowed by policy",
                         device_command);
                *error = (mcp_rpc_error_t){MCP_ERR_COMMAND_DENIED, "command denied"};
                return MCP_RESOLVE_ALLOWLIST_DENIED;
            }
            if (policy == MCP_POLICY_DENY_DESTRUCTIVE) {
                snprintf(denial_text, denial_len,
                         "destructive command '%s' denied in control profile",
                         device_command);
                *error = (mcp_rpc_error_t){MCP_ERR_COMMAND_DENIED, "command denied"};
                return MCP_RESOLVE_ALLOWLIST_DENIED;
            }
            if (policy == MCP_POLICY_DEVICE_UNAVAILABLE) {
                *error = (mcp_rpc_error_t){MCP_ERR_DEVICE_UNAVAILABLE,
                                           "device not found"};
                return MCP_RESOLVE_INVALID;
            }
            if (policy == MCP_POLICY_CAPABILITY_UNKNOWN) {
                *error = (mcp_rpc_error_t){MCP_ERR_CAPABILITY_UNKNOWN,
                                           "device capabilities not ready"};
                return MCP_RESOLVE_INVALID;
            }
            *is_device_command = true;
            return normalize_arguments(source, "device_command", device_command,
                                       msg, error);
        }

        *is_device_command = false;
        return normalize_arguments(source, "gateway_command", tool_name, msg,
                                   error);
    }

    // 2. Try dynamic catalog (§32 — tool resolution order).
    const mcp_tool_binding_t *dyn_binding = mcp_tool_catalog_find_ptr(tool_name);
    if (dyn_binding != NULL) {
        mcp_tool_binding_t binding;
        memcpy(&binding, dyn_binding, sizeof(binding));
        // Defense in depth (§34): revalidate exposure is ENABLED.
        mcp_tool_exposure_t exposure;
        if (mcp_tool_exposure_get(binding.device_id, binding.command,
                                  &exposure) != ESP_OK ||
            exposure.state != MCP_EXPOSURE_ENABLED) {
            *error = (mcp_rpc_error_t){-32602, "unknown tool"};
            return MCP_RESOLVE_INVALID;
        }

        // Verify device still exists.
        device_entry_t entry;
        if (device_store_get(binding.device_id, &entry) != DEVICE_STORE_OK) {
            snprintf(denial_text, denial_len,
                     "Device is currently unavailable");
            *error = (mcp_rpc_error_t){MCP_ERR_DEVICE_UNAVAILABLE,
                                       "device not found"};
            return MCP_RESOLVE_ALLOWLIST_DENIED;
        }

        // Verify capability still matches (§36 — semantic digest).
        device_capability_snapshot_t cap;
        if (device_capabilities_get(binding.device_id, &cap) != ESP_OK ||
            !cap.has_committed || cap.count == 0) {
            snprintf(denial_text, denial_len,
                     "Tool capability changed and requires review");
            *error = (mcp_rpc_error_t){MCP_ERR_CAPABILITY_UNKNOWN,
                                       "capabilities not ready"};
            return MCP_RESOLVE_ALLOWLIST_DENIED;
        }

        const device_capability_t *target = NULL;
        for (size_t i = 0; i < cap.count; i++) {
            if (strcmp(cap.items[i].command, binding.command) == 0) {
                target = &cap.items[i];
                break;
            }
        }
        if (target == NULL) {
            snprintf(denial_text, denial_len,
                     "Tool capability changed and requires review");
            *error = (mcp_rpc_error_t){MCP_ERR_CAPABILITY_UNKNOWN,
                                       "command missing"};
            return MCP_RESOLVE_ALLOWLIST_DENIED;
        }

        // Verify semantic digest still matches.
        uint8_t current_digest[MCP_CAPABILITY_DIGEST_LEN];
        mcp_tool_digest_compute(target, current_digest);
        if (!mcp_tool_digest_match(current_digest, exposure.capability_digest)) {
            snprintf(denial_text, denial_len,
                     "Tool capability changed and requires review");
            *error = (mcp_rpc_error_t){MCP_ERR_CAPABILITY_UNKNOWN,
                                       "capability changed"};
            return MCP_RESOLVE_ALLOWLIST_DENIED;
        }

        // Build gw_message_t directly from binding (§33 — no normalize_arguments).
        *is_device_command = true;
        msg->protocol_version = GW_PROTOCOL_VERSION;
        strlcpy(msg->type, "device_command", sizeof(msg->type));
        strlcpy(msg->device_id, binding.device_id, sizeof(msg->device_id));
        strlcpy(msg->command, binding.command, sizeof(msg->command));
        msg->has_device_id = true;

        // Dynamic argument normalization (§33): map value → int_value/bool_value.
        const cJSON *value_item = cJSON_GetObjectItemCaseSensitive(source, "value");
        if (target->value_type == DEVICE_CAP_VALUE_BOOL) {
            if (value_item != NULL && cJSON_IsBool(value_item)) {
                msg->has_bool_value = true;
                msg->bool_value = cJSON_IsTrue(value_item);
            } else if (value_item != NULL) {
                *error = (mcp_rpc_error_t){-32602, "value must be a boolean"};
                return MCP_RESOLVE_INVALID;
            }
        } else if (target->value_type == DEVICE_CAP_VALUE_INT) {
            if (value_item != NULL && cJSON_IsNumber(value_item)) {
                msg->has_int_value = true;
                msg->int_value = value_item->valueint;
            } else if (value_item != NULL) {
                *error = (mcp_rpc_error_t){-32602, "value must be an integer"};
                return MCP_RESOLVE_INVALID;
            }
        }
        // DEVICE_CAP_VALUE_NONE: no value argument expected.

        // Validate arguments (§34 — defense in depth).
        device_capability_t validated_cap;
        device_cap_validation_t val =
            device_capabilities_validate_command(msg, &validated_cap);
        if (val == DEVICE_CAP_VALID_UNKNOWN) {
            snprintf(denial_text, denial_len,
                     "Device is currently unavailable");
            *error = (mcp_rpc_error_t){MCP_ERR_DEVICE_UNAVAILABLE,
                                       "device unavailable"};
            return MCP_RESOLVE_ALLOWLIST_DENIED;
        }
        if (val == DEVICE_CAP_VALID_UNSUPPORTED_COMMAND) {
            snprintf(denial_text, denial_len,
                     "Tool capability changed and requires review");
            *error = (mcp_rpc_error_t){MCP_ERR_CAPABILITY_UNKNOWN,
                                       "command not supported"};
            return MCP_RESOLVE_ALLOWLIST_DENIED;
        }
        if (val == DEVICE_CAP_VALID_ARGUMENT) {
            *error = (mcp_rpc_error_t){-32602, "invalid argument"};
            snprintf(denial_text, denial_len,
                     "Invalid argument for command '%s'", binding.command);
            return MCP_RESOLVE_ALLOWLIST_DENIED;
        }

        return MCP_RESOLVE_OK;
    }

    // 3. Unknown tool.
    *error = (mcp_rpc_error_t){-32602, "unknown tool"};
    return MCP_RESOLVE_INVALID;
}

// ---------------------------------------------------------------------------
// Execution + wire formatting — dual-era (§10, §11)
// ---------------------------------------------------------------------------

cJSON *mcp_tools_format_dispatch(const dispatch_result_t *result,
                                 const mcp_request_context_t *ctx,
                                 mcp_rpc_error_t *err)
{
    bool ok = dispatch_result_is_ok(result);

    // Determine text content: TEXT uses payload directly, JSON serializes
    const char *text = result->format == DISPATCH_RESULT_TEXT
                           ? result->payload
                           : NULL;

    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        *err = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }

    // Build content array with text fallback (always present, §10.1 / §10.2)
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

    // For JSON results, serialize to text (§10.1)
    if (result->format == DISPATCH_RESULT_JSON && result->payload[0] != '\0') {
        cJSON_AddStringToObject(item, "text", result->payload);
    } else if (text != NULL) {
        cJSON_AddStringToObject(item, "text", text);
    } else {
        cJSON_AddStringToObject(item, "text", "");
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

    // structuredContent: 2025 only when JSON root is object (§10.2)
    // 2026: add for any JSON result
    if (result->format == DISPATCH_RESULT_JSON && result->payload[0] != '\0') {
        cJSON *parsed = cJSON_Parse(result->payload);
        if (parsed != NULL) {
            if (ctx->era == MCP_ERA_2026_07_28) {
                cJSON_AddItemToObject(out, "structuredContent", parsed);
            } else if (cJSON_IsObject(parsed)) {
                // 2025: only object roots (§10.2 Policy A)
                cJSON_AddItemToObject(out, "structuredContent", parsed);
            } else {
                // 2025: array/scalar -> text fallback only, omit structuredContent
                cJSON_Delete(parsed);
            }
        }
    }

    return out;
}

cJSON *mcp_tools_tool_error(const char *text, const mcp_request_context_t *ctx,
                            mcp_rpc_error_t *err)
{
    dispatch_result_t synthetic = {
        .status = DISPATCH_STATUS_INVALID_ARGUMENT,
        .format = DISPATCH_RESULT_TEXT,
        .payload = {0},
    };
    strlcpy(synthetic.payload, text, sizeof(synthetic.payload));
    return mcp_tools_format_dispatch(&synthetic, ctx, err);
}

cJSON *mcp_tools_execute(const gw_message_t *msg,
                         const mcp_request_context_t *ctx,
                         mcp_rpc_error_t *error)
{
    SemaphoreHandle_t mutex = ensure_dispatch_mutex();
    if (mutex == NULL) {
        *error = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    command_dispatcher_handle(msg, &s_dispatch_result);
    cJSON *result =
        mcp_tools_format_dispatch(&s_dispatch_result, ctx, error);
    xSemaphoreGive(mutex);
    return result;
}
