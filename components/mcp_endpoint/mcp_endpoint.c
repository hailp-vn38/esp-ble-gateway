#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "command_executor.h"
#include "mcp_endpoint.h"
#include "mcp_endpoint_internal.h"

#define MCP_MAX_REQUEST_LEN 4096

static const char *TAG = "mcp_endpoint";

// Default transport hooks -> real esp_http_server implementation.
static int default_recv(httpd_req_t *req, char *buf, int buf_len)
{
    return httpd_req_recv(req, buf, buf_len);
}

static esp_err_t default_send(httpd_req_t *req, const char *buf, size_t len)
{
    return httpd_resp_send(req, buf, len);
}

static esp_err_t default_send_err(httpd_req_t *req, int status,
                                  const char *message)
{
    return httpd_resp_send_err(req, (httpd_err_code_t)status, message);
}

static esp_err_t default_set_type(httpd_req_t *req, const char *type)
{
    return httpd_resp_set_type(req, type);
}

static esp_err_t default_set_status(httpd_req_t *req, const char *status)
{
    return httpd_resp_set_status(req, status);
}

static esp_err_t default_set_hdr(httpd_req_t *req, const char *field,
                                 const char *value)
{
    return httpd_resp_set_hdr(req, field, value);
}

static char *default_get_header(httpd_req_t *req, const char *name)
{
    size_t len = 0;
    if (httpd_req_get_hdr_value_len(req, name) == 0) return NULL;
    len = httpd_req_get_hdr_value_len(req, name) + 1;
    char *value = malloc(len);
    if (value == NULL) return NULL;
    if (httpd_req_get_hdr_value_str(req, name, value, len) != ESP_OK) {
        free(value);
        return NULL;
    }
    return value;
}

static esp_err_t default_async_begin(httpd_req_t *req, httpd_req_t **out)
{
    return httpd_req_async_handler_begin(req, out);
}

static esp_err_t default_async_complete(httpd_req_t *req)
{
    return httpd_req_async_handler_complete(req);
}

static const mcp_transport_t s_default_transport = {
    .recv = default_recv,
    .send = default_send,
    .send_err = default_send_err,
    .set_type = default_set_type,
    .set_status = default_set_status,
    .set_hdr = default_set_hdr,
    .get_header = default_get_header,
    .async_begin = default_async_begin,
    .async_complete = default_async_complete,
};

static mcp_transport_t s_transport;

void mcp_transport_set(const mcp_transport_t *hooks)
{
    s_transport = (hooks != NULL) ? *hooks : s_default_transport;
}

const mcp_transport_t *mcp_transport_get(void)
{
    return &s_transport;
}

typedef enum {
    MCP_RECV_OK,
    MCP_RECV_ERR_SIZE,
    MCP_RECV_ERR_MEM,
    MCP_RECV_ERR_READ,
    MCP_RECV_ERR_TIMEOUT,
} mcp_recv_status_t;

static mcp_recv_status_t receive_body(httpd_req_t *request, char **out_body)
{
    *out_body = NULL;
    if (request->content_len <= 0 || request->content_len > MCP_MAX_REQUEST_LEN) {
        return MCP_RECV_ERR_SIZE;
    }
    char *body = malloc((size_t)request->content_len + 1);
    if (body == NULL) return MCP_RECV_ERR_MEM;

    size_t received = 0;
    int timeout_retries = 0;
    while (received < (size_t)request->content_len) {
        int chunk = s_transport.recv(request, body + received,
                                     request->content_len - (int)received);
        if (chunk == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeout_retries > MCP_MAX_RECV_RETRIES) {
                free(body);
                return MCP_RECV_ERR_TIMEOUT;
            }
            continue;
        }
        if (chunk <= 0) {
            free(body);
            return MCP_RECV_ERR_READ;
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';
    *out_body = body;
    return MCP_RECV_OK;
}

// ---------------------------------------------------------------------------
// Gate error — uses exact HTTP status, no fake 500 (§12.5)
// ---------------------------------------------------------------------------

static const char *gate_status_string(mcp_gate_status_t gate)
{
    switch (gate) {
    case MCP_GATE_RATE_LIMITED:    return "429 Too Many Requests";
    case MCP_GATE_UNAUTHORIZED:    return "401 Unauthorized";
    case MCP_GATE_FORBIDDEN_HOST:  return "403 Forbidden";
    case MCP_GATE_BAD_CONTENT_TYPE: return "415 Unsupported Media Type";
    case MCP_GATE_BAD_ACCEPT:      return "406 Not Acceptable";
    case MCP_GATE_OK:              return NULL;
    }
    return "400 Bad Request";
}

static esp_err_t send_gate_error(httpd_req_t *request, mcp_gate_status_t gate)
{
    const char *status = gate_status_string(gate);
    if (status == NULL) return ESP_OK;

    const mcp_transport_t *io = mcp_transport_get();
    io->set_status(request, status);
    io->set_hdr(request, "Connection", "close");
    if (gate == MCP_GATE_UNAUTHORIZED) {
        io->set_hdr(request, "WWW-Authenticate", "Bearer");
    }
    // Use plain-text error body (§12.5) — no JSON-RPC envelope for gate errors.
    return io->send(request, status, HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// Async command context
// ---------------------------------------------------------------------------

typedef struct {
    httpd_req_t *request;
    cJSON *id;  // NULL for notifications; ownership moves to the callback
    mcp_request_context_t ctx;
    bool notification;
} mcp_command_context_t;

static void mcp_device_command_completion(const dispatch_result_t *result,
                                          void *arg)
{
    mcp_command_context_t *context = arg;

    if (!context->notification) {
        mcp_rpc_error_t rpc_error = {0};
        cJSON *payload =
            mcp_tools_format_dispatch(result, &context->ctx, &rpc_error);
        if (payload != NULL) {
            mcp_rpc_send_result_ex(context->request, payload, context->id,
                                   &context->ctx);
        } else {
            mcp_rpc_send_error_ex(
                context->request,
                rpc_error.code != 0 ? rpc_error.code : -32603,
                rpc_error.message != NULL ? rpc_error.message : "Internal error",
                context->id, &context->ctx, NULL, false);
        }
    }

    if (context->id != NULL) cJSON_Delete(context->id);
    if (mcp_transport_get()->async_complete(context->request) != ESP_OK) {
        ESP_LOGD(TAG, "async_complete failed (client gone?)");
    }
    free(context);
}

// ---------------------------------------------------------------------------
// tools/call handler
// ---------------------------------------------------------------------------

static esp_err_t handle_tools_call(httpd_req_t *request, cJSON *root,
                                   const cJSON *id, bool notification,
                                   const mcp_request_context_t *ctx)
{
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    gw_message_t message;
    memset(&message, 0, sizeof(message));
    bool is_device_command = false;
    char denial[96];
    mcp_rpc_error_t rpc_error = {0};

    mcp_resolve_status_t resolve =
        mcp_tools_resolve(params, &message, &is_device_command, denial,
                          sizeof(denial), &rpc_error);
    if (resolve == MCP_RESOLVE_INVALID) {
        return mcp_rpc_send_error_ex(request, rpc_error.code, rpc_error.message,
                                     id, ctx, NULL, false);
    }

    if (is_device_command && resolve != MCP_RESOLVE_ALLOWLIST_DENIED) {
        cJSON *id_copy = id != NULL ? cJSON_Duplicate(id, true) : NULL;
        if (id != NULL && id_copy == NULL) {
            return mcp_rpc_send_error_ex(request, -32603, "Internal error", id,
                                         ctx, NULL, false);
        }

        esp_err_t begin_error = s_transport.async_begin(request, &request);
        if (begin_error == ESP_OK) {
            mcp_command_context_t *context = malloc(sizeof(*context));
            if (context != NULL) {
                context->request = request;
                context->id = id_copy;
                context->ctx = *ctx;
                context->notification = notification;

                esp_err_t submitted = command_executor_submit(
                    &message, mcp_device_command_completion, context);
                if (submitted == ESP_OK) {
                    return ESP_OK;
                }

                s_transport.async_complete(request);
                if (id_copy != NULL) cJSON_Delete(id_copy);
                free(context);
                if (submitted == ESP_ERR_NO_MEM) {
                    return mcp_rpc_send_error_ex(
                        request, MCP_ERR_GATEWAY_BUSY,
                        "Busy: command queue full", id, ctx,
                        "503 Service Unavailable", false);
                }
                if (id != NULL) {
                    return mcp_rpc_send_error_ex(
                        request, -32603, "Executor unavailable", id, ctx,
                        "503 Service Unavailable", false);
                }
                return ESP_OK;
            } else {
                s_transport.async_complete(request);
                cJSON_Delete(id_copy);
                if (id != NULL) {
                    return mcp_rpc_send_error_ex(
                        request, -32603, "Out of memory", id, ctx,
                        "503 Service Unavailable", false);
                }
                return ESP_OK;
            }
        } else {
            cJSON_Delete(id_copy);
            if (id != NULL) {
                return mcp_rpc_send_error_ex(
                    request, -32603, "Async unavailable", id, ctx,
                    "503 Service Unavailable", false);
            }
            return ESP_OK;
        }
    }

    if (resolve == MCP_RESOLVE_ALLOWLIST_DENIED) {
        if (notification) {
            return mcp_rpc_send_accepted(request);
        }
        mcp_rpc_error_t tool_err = {0};
        cJSON *result = mcp_tools_tool_error(denial, ctx, &tool_err);
        if (result == NULL) {
            return mcp_rpc_send_error_ex(request, tool_err.code,
                                         tool_err.message, id, ctx, NULL,
                                         false);
        }
        return mcp_rpc_send_result_ex(request, result, id, ctx);
    }

    cJSON *result = mcp_tools_execute(&message, ctx, &rpc_error);
    if (result == NULL) {
        return mcp_rpc_send_error_ex(request, rpc_error.code, rpc_error.message,
                                     id, ctx, NULL, false);
    }
    if (notification) {
        cJSON_Delete(result);
        return mcp_rpc_send_accepted(request);
    }
    return mcp_rpc_send_result_ex(request, result, id, ctx);
}

// ---------------------------------------------------------------------------
// Route method (§16)
// ---------------------------------------------------------------------------

static esp_err_t route_request(httpd_req_t *request, cJSON *root,
                               const cJSON *id, bool notification,
                               const mcp_request_context_t *ctx)
{
    const cJSON *method_item = cJSON_GetObjectItemCaseSensitive(root, "method");
    const char *method = method_item->valuestring;

    // 2025 era: initialize + notifications/initialized
    if (ctx->era == MCP_ERA_2025_11_25) {
        if (ctx->initialize_request) {
            mcp_rpc_error_detail_t error;
            mcp_rpc_error_detail_init(&error);
            cJSON *result = mcp_protocol_build_initialize_result(
                cJSON_GetObjectItemCaseSensitive(root, "params"), &error);
            if (result == NULL) {
                return mcp_rpc_send_error_detail(request, &error, id, false);
            }
            return mcp_rpc_send_result_ex(request, result, id, ctx);
        }

        if (strcmp(method, "notifications/initialized") == 0) {
            // §8.3: 202 Accepted, empty body, header optional
            return mcp_rpc_send_accepted(request);
        }
    }

    // 2026 era: server/discover
    if (ctx->era == MCP_ERA_2026_07_28 &&
        strcmp(method, "server/discover") == 0) {
        cJSON *result = mcp_codec_build_discovery();
        if (result == NULL) {
            return mcp_rpc_send_error(request, -32603, "Internal error", id);
        }
        if (notification) {
            cJSON_Delete(result);
            return mcp_rpc_send_accepted(request);
        }
        return mcp_rpc_send_result_ex(request, result, id, ctx);
    }

    // Shared: tools/list, tools/call
    if (strcmp(method, "tools/list") == 0) {
        cJSON *result = mcp_tools_list(ctx);
        if (result == NULL) {
            return mcp_rpc_send_error(request, -32603, "Internal error", id);
        }
        if (notification) {
            cJSON_Delete(result);
            return mcp_rpc_send_accepted(request);
        }
        return mcp_rpc_send_result_ex(request, result, id, ctx);
    }

    if (strcmp(method, "tools/call") == 0) {
        return handle_tools_call(request, root, id, notification, ctx);
    }

    // Unknown method
    if (ctx->era == MCP_ERA_2026_07_28) {
        return mcp_rpc_send_error_ex(request, -32601, "Method not found",
                                     id, ctx, "404 Not Found", false);
    }
    return mcp_rpc_send_error(request, -32601, "Method not found", id);
}

// ---------------------------------------------------------------------------
// POST /mcp handler — pipeline §6
// ---------------------------------------------------------------------------

esp_err_t mcp_handle_request(httpd_req_t *request)
{
    // Step 1-2: HTTP security + transport gate
    mcp_gate_status_t gate = mcp_auth_gate(request);
    if (gate != MCP_GATE_OK) {
        return send_gate_error(request, gate);
    }

    // Step 3: Receive body (moved BEFORE protocol detection, §6)
    char *body = NULL;
    mcp_recv_status_t recv_status = receive_body(request, &body);
    if (recv_status != MCP_RECV_OK) {
        const char *message = "Parse error";
        int code = -32700;
        const char *http_status = NULL;
        if (recv_status == MCP_RECV_ERR_SIZE) {
            message = "Invalid Request: body exceeds limit";
            code = -32600;
            http_status = "413 Content Too Large";
        } else if (recv_status == MCP_RECV_ERR_MEM) {
            message = "Internal error";
            code = -32603;
        }
        bool close_conn = (recv_status == MCP_RECV_ERR_SIZE);
        return mcp_rpc_send_error_ex(request, code, message, NULL, NULL,
                                     http_status, close_conn);
    }

    // Step 4: Parse JSON
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return mcp_rpc_send_error(request, -32700, "Parse error", NULL);
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return mcp_rpc_send_error(request, -32600, "Invalid Request", NULL);
    }

    // Step 5: Generic JSON-RPC validation
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "jsonrpc");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");

    if (!cJSON_IsString(version) || version->valuestring == NULL ||
        strcmp(version->valuestring, "2.0") != 0 ||
        !cJSON_IsString(method) || method->valuestring == NULL ||
        method->valuestring[0] == '\0') {
        esp_err_t outcome = mcp_rpc_send_error_ex(request, -32600,
                                                  "Invalid Request", id, NULL,
                                                  NULL, false);
        cJSON_Delete(root);
        return outcome;
    }

    // id:null is rejected (§12.2)
    if (id != NULL && cJSON_IsNull(id)) {
        esp_err_t outcome = mcp_rpc_send_error(request, -32600,
                                               "Invalid Request", id);
        cJSON_Delete(root);
        return outcome;
    }

    bool notification = (id == NULL);

    // Step 6: Detect protocol era (§7.2)
    mcp_request_context_t ctx;
    mcp_rpc_error_detail_t detect_error;
    mcp_rpc_error_detail_init(&detect_error);

    int detect_rc = mcp_protocol_detect(request, root, &ctx, &detect_error);
    if (detect_rc != 0) {
        esp_err_t outcome =
            mcp_rpc_send_error_detail(request, &detect_error, id, false);
        cJSON_Delete(root);
        return outcome;
    }

    // Step 7: Era-specific validation
    mcp_rpc_error_detail_t validate_error;
    mcp_rpc_error_detail_init(&validate_error);

    int validate_rc = mcp_protocol_validate_request(root, &ctx, &validate_error);
    if (validate_rc != 0) {
        esp_err_t outcome =
            mcp_rpc_send_error_detail(request, &validate_error, id, false);
        cJSON_Delete(root);
        return outcome;
    }

    // Step 8: Route method
    esp_err_t outcome = route_request(request, root, id, notification, &ctx);

    cJSON_Delete(root);
    return outcome;
}

// ---------------------------------------------------------------------------
// GET /mcp — 405 (§13.2)
// ---------------------------------------------------------------------------

static esp_err_t handle_get_mcp(httpd_req_t *request)
{
    const mcp_transport_t *io = mcp_transport_get();
    io->set_status(request, "405 Method Not Allowed");
    io->set_hdr(request, "Allow", "POST");
    io->set_hdr(request, "Connection", "close");
    return io->send(request, NULL, 0);
}

// ---------------------------------------------------------------------------
// DELETE /mcp — 405 (§13.3)
// ---------------------------------------------------------------------------

static esp_err_t handle_delete_mcp(httpd_req_t *request)
{
    const mcp_transport_t *io = mcp_transport_get();
    io->set_status(request, "405 Method Not Allowed");
    io->set_hdr(request, "Allow", "POST");
    io->set_hdr(request, "Connection", "close");
    return io->send(request, NULL, 0);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

int mcp_endpoint_register(httpd_handle_t server)
{
    if (server == NULL) return -1;
    mcp_transport_set(NULL);

    const httpd_uri_t post_route = {
        .uri = "/mcp",
        .method = HTTP_POST,
        .handler = mcp_handle_request,
        .user_ctx = NULL,
    };
    esp_err_t error = httpd_register_uri_handler(server, &post_route);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not register POST /mcp: %s", esp_err_to_name(error));
        return -1;
    }

    const httpd_uri_t get_route = {
        .uri = "/mcp",
        .method = HTTP_GET,
        .handler = handle_get_mcp,
        .user_ctx = NULL,
    };
    error = httpd_register_uri_handler(server, &get_route);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not register GET /mcp: %s", esp_err_to_name(error));
        return -1;
    }

    const httpd_uri_t delete_route = {
        .uri = "/mcp",
        .method = HTTP_DELETE,
        .handler = handle_delete_mcp,
        .user_ctx = NULL,
    };
    error = httpd_register_uri_handler(server, &delete_route);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not register DELETE /mcp: %s", esp_err_to_name(error));
        return -1;
    }

    ESP_LOGI(TAG, "MCP endpoint registered: POST/GET/DELETE /mcp");
    return 0;
}
