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
#include "mcp_endpoint_internal.h"

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
// tools/list
// ---------------------------------------------------------------------------

cJSON *mcp_tools_list(const mcp_request_meta_t *meta)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool_names = cJSON_CreateArray();
    if (result == NULL || tools == NULL || tool_names == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(tools);
        cJSON_Delete(tool_names);
        return NULL;
    }
    if (meta->mcp_2026) {
        // Cache hints required by the 2026-07-28 ListToolsResult.
        cJSON_AddNumberToObject(result, "ttlMs", MCP_TOOLS_CACHE_TTL_MS);
        cJSON_AddStringToObject(result, "cacheScope", MCP_TOOLS_CACHE_SCOPE);
    }

    if (mcp_registry_build_tools_list(tools, tool_names) != 0) {
        cJSON_Delete(result);
        cJSON_Delete(tools);
        cJSON_Delete(tool_names);
        return NULL;
    }
    cJSON_AddItemToObject(result, "tools", tools);
    cJSON_AddItemToObject(result, "tool_names", tool_names);
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
    // Preserve the historical integer `value` alias while publishing the
    // canonical int_value/bool_value fields in the tool schema.
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

    const mcp_tool_desc_t *desc = mcp_registry_find(tool_name);
    if (desc == NULL) {
        // Strict surface: unknown names are protocol errors. The old
        // unknown-tool + device_id -> device_command fallback is gone.
        *error = (mcp_rpc_error_t){-32602, "unknown tool"};
        return MCP_RESOLVE_INVALID;
    }

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
            *error = (mcp_rpc_error_t){0};
            return MCP_RESOLVE_ALLOWLIST_DENIED;
        }
        *is_device_command = true;
        return normalize_arguments(source, "device_command", device_command,
                                   msg, error);
    }

    // Gateway tools: the MCP tool name IS the gateway command.
    *is_device_command = false;
    return normalize_arguments(source, "gateway_command", tool_name, msg,
                               error);
}

// ---------------------------------------------------------------------------
// Execution + wire formatting
// ---------------------------------------------------------------------------

cJSON *mcp_tools_format_dispatch(const dispatch_result_t *result,
                                 const mcp_request_meta_t *meta,
                                 mcp_rpc_error_t *err)
{
    bool ok = dispatch_result_is_ok(result);
    const char *text = result->format == DISPATCH_RESULT_TEXT
                           ? result->payload
                           : "";
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        *err = (mcp_rpc_error_t){-32603, "out of memory"};
        return NULL;
    }
    if (meta->mcp_2026) {
        cJSON_AddStringToObject(out, "resultType", "complete");
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
        cJSON_AddStringToObject(item, "text", text);
        cJSON_AddItemToArray(content, item);
        cJSON_AddItemToObject(out, "content", content);
        cJSON_AddBoolToObject(out, "isError", !ok);
    } else {
        cJSON_AddBoolToObject(out, "success", ok);
        cJSON_AddNumberToObject(out, "status", (int)result->status);
        cJSON_AddStringToObject(out, "message", text);
    }

    if (result->format == DISPATCH_RESULT_JSON) {
        cJSON *data = cJSON_Parse(result->payload);
        if (data == NULL) {
            cJSON_Delete(out);
            *err = (mcp_rpc_error_t){-32603, "dispatcher returned invalid JSON"};
            return NULL;
        }
        const char *field = meta->mcp_2026 ? "structuredContent" : "data";
        cJSON_AddItemToObject(out, field, data);
    }
    return out;
}

cJSON *mcp_tools_tool_error(const char *text, const mcp_request_meta_t *meta,
                            mcp_rpc_error_t *err)
{
    dispatch_result_t synthetic = {
        .status = DISPATCH_STATUS_INVALID_ARGUMENT,
        .format = DISPATCH_RESULT_TEXT,
        .payload = {0},
    };
    strlcpy(synthetic.payload, text, sizeof(synthetic.payload));
    return mcp_tools_format_dispatch(&synthetic, meta, err);
}

cJSON *mcp_tools_execute(const gw_message_t *msg,
                         const mcp_request_meta_t *meta,
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
        mcp_tools_format_dispatch(&s_dispatch_result, meta, error);
    xSemaphoreGive(mutex);
    return result;
}
