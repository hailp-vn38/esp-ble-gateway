#include "mcp_core.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "command_executor.h"
#include "device_command_service.h"
#include "esp_log.h"

#include "mcp_endpoint_internal.h"

static const char *TAG = "mcp_core";

typedef struct {
    cJSON *id;
    mcp_request_context_t protocol;
    mcp_responder_t responder;
    bool notification;
} mcp_async_context_t;

static bool responder_valid(const mcp_responder_t *responder)
{
    return responder != NULL && responder->send_json != NULL &&
           responder->send_none != NULL;
}

static bool responder_alive(const mcp_responder_t *responder)
{
    return responder->is_alive == NULL ||
           responder->is_alive(responder->context);
}

static void responder_release(mcp_responder_t *responder)
{
    if (responder->release != NULL) responder->release(responder->context);
    memset(responder, 0, sizeof(*responder));
}

static cJSON *duplicate_id(const cJSON *id)
{
    if (id == NULL) return cJSON_CreateNull();
    if (!cJSON_IsString(id) && !cJSON_IsNumber(id)) return cJSON_CreateNull();
    return cJSON_Duplicate(id, true);
}

static esp_err_t send_envelope(const mcp_responder_t *responder,
                               cJSON *envelope,
                               const mcp_response_meta_t *meta,
                               const char *diagnostic_method,
                               size_t diagnostic_item_count)
{
    if (envelope == NULL) return ESP_ERR_NO_MEM;
    char *json = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    if (json == NULL) return ESP_ERR_NO_MEM;
    size_t json_len = strlen(json);
    if (diagnostic_method != NULL) {
        ESP_LOGD(TAG, "MCP %s: tools=%u json=%u bytes", diagnostic_method,
                 (unsigned)diagnostic_item_count, (unsigned)json_len);
    }
    esp_err_t result = responder_alive(responder)
                           ? responder->send_json(responder->context, json,
                                                  json_len, meta)
                           : ESP_ERR_INVALID_STATE;
    cJSON_free(json);
    return result;
}

static cJSON *build_envelope(const cJSON *id)
{
    cJSON *response_id = duplicate_id(id);
    cJSON *envelope = response_id != NULL ? cJSON_CreateObject() : NULL;
    if (envelope == NULL) {
        cJSON_Delete(response_id);
        return NULL;
    }
    cJSON_AddStringToObject(envelope, "jsonrpc", "2.0");
    cJSON_AddItemToObject(envelope, "id", response_id);
    return envelope;
}

static esp_err_t send_result(const mcp_responder_t *responder, cJSON *result,
                             const cJSON *id)
{
    cJSON *envelope = build_envelope(id);
    if (envelope == NULL || result == NULL ||
        !cJSON_AddItemToObject(envelope, "result", result)) {
        cJSON_Delete(result);
        cJSON_Delete(envelope);
        return ESP_ERR_NO_MEM;
    }
    return send_envelope(responder, envelope, NULL, NULL, 0);
}

static esp_err_t send_tools_list_result(const mcp_responder_t *responder,
                                        cJSON *result, const cJSON *id)
{
    cJSON *tools = result != NULL
                       ? cJSON_GetObjectItemCaseSensitive(result, "tools")
                       : NULL;
    size_t tool_count = cJSON_IsArray(tools)
                            ? (size_t)cJSON_GetArraySize(tools)
                            : 0;
    cJSON *envelope = build_envelope(id);
    if (envelope == NULL || result == NULL ||
        !cJSON_AddItemToObject(envelope, "result", result)) {
        cJSON_Delete(result);
        cJSON_Delete(envelope);
        return ESP_ERR_NO_MEM;
    }
    return send_envelope(responder, envelope, NULL, "tools/list", tool_count);
}

static esp_err_t send_error_detail(const mcp_responder_t *responder,
                                   int code, const char *message,
                                   const cJSON *id,
                                   mcp_rpc_error_detail_t *detail,
                                   const char *http_status)
{
    cJSON *envelope = build_envelope(id);
    cJSON *error = envelope != NULL ? cJSON_CreateObject() : NULL;
    if (error == NULL) {
        cJSON_Delete(envelope);
        if (detail != NULL) mcp_rpc_error_detail_clear(detail);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message",
                           message != NULL ? message : "Internal error");
    if (detail != NULL && detail->data != NULL) {
        cJSON_AddItemToObject(error, "data", detail->data);
        detail->data = NULL;
    }
    cJSON_AddItemToObject(envelope, "error", error);
    const mcp_response_meta_t meta = {.http_status = http_status};
    return send_envelope(responder, envelope, &meta, NULL, 0);
}

static esp_err_t send_error(const mcp_responder_t *responder, int code,
                            const char *message, const cJSON *id,
                            const char *http_status)
{
    return send_error_detail(responder, code, message, id, NULL, http_status);
}

static esp_err_t send_none(const mcp_responder_t *responder)
{
    const mcp_response_meta_t meta = {.http_status = "202 Accepted"};
    return responder_alive(responder)
               ? responder->send_none(responder->context, &meta)
               : ESP_ERR_INVALID_STATE;
}

static void device_command_completion(const device_command_result_t *result,
                                      void *arg)
{
    mcp_async_context_t *context = arg;
    if (!context->notification && responder_alive(&context->responder)) {
        mcp_rpc_error_t error = {0};
        cJSON *payload =
            mcp_tools_format_device_result(result, &context->protocol, &error);
        if (payload != NULL) {
            send_result(&context->responder, payload, context->id);
        } else {
            send_error(&context->responder,
                       error.code != 0 ? error.code : -32603,
                       error.message != NULL ? error.message : "Internal error",
                       context->id, NULL);
        }
    }
    cJSON_Delete(context->id);
    responder_release(&context->responder);
    free(context);
}

static esp_err_t handle_tools_call(const mcp_responder_t *responder,
                                   cJSON *root, const cJSON *id,
                                   bool notification,
                                   const mcp_request_context_t *protocol)
{
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    gw_message_t message = {0};
    mcp_tool_exec_kind_t exec_kind = MCP_TOOL_EXEC_GATEWAY_SYNC;
    char denial[96] = {0};
    mcp_rpc_error_t error = {0};
    mcp_resolve_status_t resolution =
        mcp_tools_resolve(params, &message, &exec_kind, denial,
                          sizeof(denial), &error);
    if (resolution == MCP_RESOLVE_INVALID) {
        return send_error(responder, error.code, error.message, id, NULL);
    }

    if (exec_kind == MCP_TOOL_EXEC_DEVICE_SERVICE && resolution != MCP_RESOLVE_ALLOWLIST_DENIED) {
        if (responder->clone == NULL) {
            return notification ? ESP_ERR_INVALID_STATE
                                : send_error(responder, -32603,
                                             "Async unavailable", id,
                                             "503 Service Unavailable");
        }
        mcp_async_context_t *async = calloc(1, sizeof(*async));
        if (async == NULL) {
            return notification ? ESP_ERR_NO_MEM
                                : send_error(responder, -32603,
                                             "Out of memory", id,
                                             "503 Service Unavailable");
        }
        async->id = id != NULL ? cJSON_Duplicate(id, true) : NULL;
        if (id != NULL && async->id == NULL) {
            free(async);
            return send_error(responder, -32603, "Out of memory", id,
                              "503 Service Unavailable");
        }
        async->protocol = *protocol;
        async->notification = notification;
        esp_err_t clone_result = responder->clone(responder, &async->responder);
        if (clone_result != ESP_OK) {
            cJSON_Delete(async->id);
            free(async);
            return notification ? clone_result
                                : send_error(responder, -32603,
                                             "Async unavailable", id,
                                             "503 Service Unavailable");
        }

        /* Build typed service request from resolved message */
        device_command_request_t svc_req = {0};
        svc_req.origin = DEVICE_CMD_ORIGIN_CONTROL;
        strlcpy(svc_req.device_id, message.device_id, sizeof(svc_req.device_id));
        strlcpy(svc_req.command, message.command, sizeof(svc_req.command));
        if (message.has_bool_value) {
            svc_req.bool_value = message.bool_value != 0;
            svc_req.has_bool_value = true;
        }
        if (message.has_int_value) {
            svc_req.int_value = message.int_value;
            svc_req.has_int_value = true;
        }
        if (message.has_feature_id) {
            strlcpy(svc_req.feature_id, message.feature_id, sizeof(svc_req.feature_id));
            svc_req.has_feature_id = true;
        }
        if (message.has_property_id) {
            svc_req.property_id = message.property_id;
            svc_req.has_property_id = true;
        }

        esp_err_t submitted = device_command_service_submit(
            &svc_req, device_command_completion, async);
        if (submitted == ESP_OK) return ESP_OK;

        responder_release(&async->responder);
        cJSON_Delete(async->id);
        free(async);
        if (notification) return submitted;
        return send_error(responder,
                          submitted == ESP_ERR_NO_MEM ? MCP_ERR_GATEWAY_BUSY
                                                      : -32603,
                          submitted == ESP_ERR_NO_MEM
                              ? "Busy: command queue full"
                              : "Service unavailable",
                          id, "503 Service Unavailable");
    }

    if (resolution == MCP_RESOLVE_ALLOWLIST_DENIED) {
        if (notification) return send_none(responder);
        mcp_rpc_error_t tool_error = {0};
        cJSON *result = mcp_tools_tool_error(denial, protocol, &tool_error);
        return result != NULL
                   ? send_result(responder, result, id)
                   : send_error(responder, tool_error.code,
                                tool_error.message, id, NULL);
    }

    /* Local semantic tools (device_control) — no gw_message_t needed */
    if (exec_kind == MCP_TOOL_EXEC_LOCAL) {
        const cJSON *tool_args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        const cJSON *local_params = cJSON_IsObject(tool_args) ? tool_args : params;

        mcp_rpc_error_t local_err = {0};
        cJSON *result = mcp_device_control_execute(local_params, protocol,
                                                   &local_err);
        if (result == NULL) {
            if (local_err.code == 0) {
                /* Async path — device_control dispatches internally */
                if (responder->clone == NULL) {
                    return notification ? ESP_ERR_INVALID_STATE
                                        : send_error(responder, -32603,
                                                     "Async unavailable", id,
                                                     "503 Service Unavailable");
                }
                mcp_async_context_t *async = calloc(1, sizeof(*async));
                if (async == NULL) {
                    return notification ? ESP_ERR_NO_MEM
                                        : send_error(responder, -32603,
                                                     "Out of memory", id,
                                                     "503 Service Unavailable");
                }
                async->id = id != NULL ? cJSON_Duplicate(id, true) : NULL;
                async->protocol = *protocol;
                async->notification = notification;
                esp_err_t clone_result = responder->clone(responder, &async->responder);
                if (clone_result != ESP_OK) {
                    cJSON_Delete(async->id);
                    free(async);
                    return notification ? clone_result
                                        : send_error(responder, -32603,
                                                     "Async unavailable", id,
                                                     "503 Service Unavailable");
                }
                esp_err_t submitted = mcp_device_control_dispatch_async(
                    local_params, &async->responder, async->id, &async->protocol);
                if (submitted == ESP_OK) return ESP_OK;
                responder_release(&async->responder);
                cJSON_Delete(async->id);
                free(async);
                if (notification) return submitted;
                return send_error(responder,
                                  submitted == ESP_ERR_NO_MEM ? MCP_ERR_GATEWAY_BUSY : -32603,
                                  submitted == ESP_ERR_NO_MEM ? "Busy: command queue full"
                                                              : "Service unavailable",
                                  id, "503 Service Unavailable");
            }
            return send_error(responder, local_err.code, local_err.message, id, NULL);
        }
        if (notification) {
            cJSON_Delete(result);
            return send_none(responder);
        }
        return send_result(responder, result, id);
    }

    cJSON *result = mcp_tools_execute(&message, protocol, &error);
    if (result == NULL) {
        return send_error(responder, error.code, error.message, id, NULL);
    }
    if (notification) {
        cJSON_Delete(result);
        return send_none(responder);
    }
    return send_result(responder, result, id);
}

static esp_err_t route_request(const mcp_responder_t *responder, cJSON *root,
                               const cJSON *id, bool notification,
                               const mcp_request_context_t *protocol)
{
    const char *method = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "method"));

    if (protocol->initialize_request) {
        mcp_rpc_error_detail_t error;
        mcp_rpc_error_detail_init(&error);
        cJSON *result = mcp_protocol_build_initialize_result(
            cJSON_GetObjectItemCaseSensitive(root, "params"), &error);
        if (result == NULL) {
            esp_err_t outcome = send_error_detail(
                responder, error.rpc_code, error.message, id, &error,
                error.http_status);
            mcp_rpc_error_detail_clear(&error);
            return outcome;
        }
        return send_result(responder, result, id);
    }

    if ((protocol->era == MCP_ERA_2024_11_05 ||
         protocol->era == MCP_ERA_2025_11_25) &&
        strcmp(method, "notifications/initialized") == 0) {
        return send_none(responder);
    }

    if (strcmp(method, "ping") == 0) {
        if (notification) return send_none(responder);
        return send_result(responder, cJSON_CreateObject(), id);
    }

    if (protocol->era == MCP_ERA_2026_07_28 &&
        strcmp(method, "server/discover") == 0) {
        cJSON *result = mcp_codec_build_discovery();
        if (result == NULL) {
            return send_error(responder, -32603, "Internal error", id, NULL);
        }
        if (notification) {
            cJSON_Delete(result);
            return send_none(responder);
        }
        return send_result(responder, result, id);
    }

    if (strcmp(method, "tools/list") == 0) {
        cJSON *result = mcp_tools_list(protocol);
        if (result == NULL) {
            return send_error(responder, -32603, "Internal error", id, NULL);
        }
        if (notification) {
            cJSON_Delete(result);
            return send_none(responder);
        }
        return send_tools_list_result(responder, result, id);
    }

    if (strcmp(method, "tools/call") == 0) {
        return handle_tools_call(responder, root, id, notification, protocol);
    }

    return send_error(responder, -32601, "Method not found", id,
                      protocol->era == MCP_ERA_2026_07_28
                          ? "404 Not Found"
                          : NULL);
}

esp_err_t mcp_core_handle_json(const char *json, size_t json_len,
                               const mcp_wire_context_t *wire,
                               const mcp_responder_t *responder)
{
    if (json == NULL || json_len == 0 || wire == NULL ||
        !responder_valid(responder)) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (root == NULL) return send_error(responder, -32700, "Parse error", NULL, NULL);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_error(responder, -32600, "Invalid Request", NULL, NULL);
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const char *version = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "jsonrpc"));
    const char *method = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "method"));
    if (version == NULL || strcmp(version, "2.0") != 0 || method == NULL ||
        method[0] == '\0' || (id != NULL && cJSON_IsNull(id))) {
        esp_err_t outcome =
            send_error(responder, -32600, "Invalid Request", id, NULL);
        cJSON_Delete(root);
        return outcome;
    }

    mcp_request_context_t protocol;
    mcp_rpc_error_detail_t error;
    mcp_rpc_error_detail_init(&error);
    int detected = mcp_protocol_detect(root, wire, &protocol, &error);
    if (detected != 0) {
        esp_err_t outcome = send_error_detail(
            responder, error.rpc_code, error.message, id, &error,
            error.http_status);
        mcp_rpc_error_detail_clear(&error);
        cJSON_Delete(root);
        return outcome;
    }

    int valid = mcp_protocol_validate_request(root, &protocol, &error);
    if (valid != 0) {
        esp_err_t outcome = send_error_detail(
            responder, error.rpc_code, error.message, id, &error,
            error.http_status);
        mcp_rpc_error_detail_clear(&error);
        cJSON_Delete(root);
        return outcome;
    }

    esp_err_t outcome = route_request(responder, root, id, id == NULL,
                                      &protocol);
    cJSON_Delete(root);
    return outcome;
}
