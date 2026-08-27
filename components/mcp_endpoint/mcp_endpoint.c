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

static esp_err_t send_gate_error(httpd_req_t *request, mcp_gate_status_t gate)
{
    // The request body was never read: keep-alive would desync on the next
    // pipelined request, so every gate rejection closes the connection.
    const mcp_transport_t *io = mcp_transport_get();
    const char *status = "400 Bad Request";
    switch (gate) {
    case MCP_GATE_RATE_LIMITED:
        status = "429 Too Many Requests";
        break;
    case MCP_GATE_UNAUTHORIZED:
        status = "401 Unauthorized";
        break;
    case MCP_GATE_FORBIDDEN_HOST:
        status = "403 Forbidden";
        break;
    case MCP_GATE_BAD_CONTENT_TYPE:
        status = "415 Unsupported Media Type";
        break;
    case MCP_GATE_OK:
        return ESP_OK;
    }

    // Auth/security failures use plain HTTP error, no JSON-RPC envelope.
    io->set_status(request, status);
    io->set_hdr(request, "Connection", "close");
    if (gate == MCP_GATE_UNAUTHORIZED) {
        io->set_hdr(request, "WWW-Authenticate", "Bearer");
    }
    return io->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, status);
}

// Transport-specific completion context for device commands dispatched
// through the shared command executor: no MCP-owned worker or queue,
// only formatting and socket release.
typedef struct {
    httpd_req_t *request;
    cJSON *id; // NULL for notifications; ownership moves to the callback
    mcp_request_meta_t meta;
    bool notification;
} mcp_command_context_t;

static void mcp_device_command_completion(const dispatch_result_t *result,
                                          void *arg)
{
    mcp_command_context_t *context = arg;

    if (!context->notification) {
        mcp_rpc_error_t rpc_error = {0};
        cJSON *payload =
            mcp_tools_format_dispatch(result, &context->meta, &rpc_error);
        if (payload != NULL) {
            mcp_rpc_send_result_ex(context->request, payload, context->id,
                                   &context->meta);
        } else {
            mcp_rpc_send_error_ex(
                context->request,
                rpc_error.code != 0 ? rpc_error.code : -32603,
                rpc_error.message != NULL ? rpc_error.message : "Internal error",
                context->id, &context->meta, NULL, false);
        }
    }
    // Notifications answer without a response body; the socket is
    // released either way.

    if (context->id != NULL) cJSON_Delete(context->id);
    if (mcp_transport_get()->async_complete(context->request) != ESP_OK) {
        ESP_LOGD(TAG, "async_complete failed (client gone?)");
    }
    free(context);
}

static esp_err_t handle_tools_call(httpd_req_t *request, cJSON *root,
                                   const cJSON *id, bool notification,
                                   const mcp_request_meta_t *meta)
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
                                     id, meta, NULL, false);
    }

    if (is_device_command && resolve != MCP_RESOLVE_ALLOWLIST_DENIED) {
        cJSON *id_copy = id != NULL ? cJSON_Duplicate(id, true) : NULL;
        if (id != NULL && id_copy == NULL) {
            return mcp_rpc_send_error_ex(request, -32603, "Internal error", id,
                                         meta, NULL, false);
        }

        esp_err_t begin_error = s_transport.async_begin(request, &request);
        if (begin_error == ESP_OK) {
            mcp_command_context_t *context = malloc(sizeof(*context));
            if (context != NULL) {
                context->request = request;
                context->id = id_copy;
                context->meta = *meta;
                context->notification = notification;

                // Queue admission only: the executor worker dispatches and
                // the completion callback answers. Queue-full keeps 503.
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
                        "Busy: command queue full", id, meta,
                        "503 Service Unavailable", false);
                }
                // Executor unavailable entirely -> 503, no sync fallback.
                if (id != NULL) {
                    return mcp_rpc_send_error_ex(
                        request, -32603, "Executor unavailable", id, meta,
                        "503 Service Unavailable", false);
                }
                return ESP_OK;  // notification, no response needed
            } else {
                s_transport.async_complete(request);
                cJSON_Delete(id_copy);
                // OOM for context -> 503, no sync fallback.
                if (id != NULL) {
                    return mcp_rpc_send_error_ex(
                        request, -32603, "Out of memory", id, meta,
                        "503 Service Unavailable", false);
                }
                return ESP_OK;
            }
        } else {
            cJSON_Delete(id_copy);
            // Async handoff unavailable -> 503, no sync fallback.
            if (id != NULL) {
                return mcp_rpc_send_error_ex(
                    request, -32603, "Async unavailable", id, meta,
                    "503 Service Unavailable", false);
            }
            return ESP_OK;
        }
    }

    if (resolve == MCP_RESOLVE_ALLOWLIST_DENIED) {
        if (notification) {
            return mcp_rpc_send_no_content(request);
        }
        mcp_rpc_error_t tool_err = {0};
        cJSON *result = mcp_tools_tool_error(denial, meta, &tool_err);
        if (result == NULL) {
            return mcp_rpc_send_error_ex(request, tool_err.code,
                                         tool_err.message, id, meta, NULL,
                                         false);
        }
        return mcp_rpc_send_result_ex(request, result, id, meta);
    }

    cJSON *result = mcp_tools_execute(&message, meta, &rpc_error);
    if (result == NULL) {
        return mcp_rpc_send_error_ex(request, rpc_error.code, rpc_error.message,
                                     id, meta, NULL, false);
    }
    if (notification) {
        cJSON_Delete(result);
        return mcp_rpc_send_no_content(request);
    }
    return mcp_rpc_send_result_ex(request, result, id, meta);
}

esp_err_t mcp_handle_request(httpd_req_t *request)
{
    mcp_gate_status_t gate = mcp_auth_gate(request);
    if (gate != MCP_GATE_OK) {
        return send_gate_error(request, gate);
    }

    mcp_request_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    int version_check = mcp_codec_parse_meta(request, &meta);
    if (version_check != 0) {
        return mcp_rpc_send_error_ex(
            request, MCP_ERR_UNSUPPORTED_VERSION,
            "Unsupported MCP-Protocol-Version", NULL, NULL,
            "400 Bad Request", true);
    }

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
        // Size rejections skip reading the body; close to keep the socket sane.
        bool close_conn = (recv_status == MCP_RECV_ERR_SIZE);
        return mcp_rpc_send_error_ex(request, code, message, NULL, NULL,
                                     http_status, close_conn);
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return mcp_rpc_send_error(request, -32700, "Parse error", NULL);
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return mcp_rpc_send_error(request, -32600, "Invalid Request", NULL);
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "jsonrpc");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    bool valid_id = id == NULL || cJSON_IsString(id) || cJSON_IsNumber(id) ||
                    cJSON_IsNull(id);
    if (!cJSON_IsString(version) || version->valuestring == NULL ||
        strcmp(version->valuestring, "2.0") != 0 || !cJSON_IsString(method) ||
        method->valuestring == NULL || method->valuestring[0] == '\0' || !valid_id) {
        esp_err_t result = mcp_rpc_send_error_ex(request, -32600,
                                                 "Invalid Request", id, &meta,
                                                 NULL, false);
        cJSON_Delete(root);
        return result;
    }

    bool notification = id == NULL;

    // --- MCP 2026-07-28 protocol validation ---
    if (meta.mcp_2026) {
        // Validate required _meta in body
        int meta_rc = mcp_protocol_validate_meta(root);
        if (meta_rc != 0) {
            const char *errmsg = "Invalid params";
            const char *http_status = NULL;
            if (meta_rc == MCP_ERR_UNSUPPORTED_VERSION) {
                errmsg = "Unsupported protocol version";
                http_status = "400 Bad Request";
            }
            esp_err_t outcome = mcp_rpc_send_error_ex(
                request, meta_rc, errmsg, id, &meta, http_status, false);
            cJSON_Delete(root);
            return outcome;
        }

        // Validate header/body consistency (Mcp-Method match, Mcp-Name match)
        int header_rc = mcp_protocol_validate_headers(root, &meta);
        if (header_rc != 0) {
            esp_err_t outcome = mcp_rpc_send_error_ex(
                request, header_rc, "Header mismatch", id, &meta,
                "400 Bad Request", false);
            cJSON_Delete(root);
            return outcome;
        }
    }

    // --- Route to operation ---
    esp_err_t outcome;
    if (strcmp(method->valuestring, "tools/list") == 0) {
        // Legacy alias "list_tools" only accepted in legacy mode
        if (!meta.mcp_2026 &&
            strcmp(method->valuestring, "list_tools") != 0) {
            // Shouldn't reach here, but safety check
        }
        cJSON *rpc_result = mcp_tools_list(&meta);
        if (rpc_result == NULL) {
            outcome = mcp_rpc_send_error(request, -32603, "Internal error", id);
        } else if (notification) {
            cJSON_Delete(rpc_result);
            outcome = mcp_rpc_send_no_content(request);
        } else {
            outcome = mcp_rpc_send_result_ex(request, rpc_result, id, &meta);
        }
    } else if (strcmp(method->valuestring, "tools/call") == 0) {
        outcome = handle_tools_call(request, root, id, notification, &meta);
    } else if (strcmp(method->valuestring, "server/discover") == 0) {
        cJSON *rpc_result = mcp_codec_build_discovery();
        if (rpc_result == NULL) {
            outcome = mcp_rpc_send_error(request, -32603, "Internal error", id);
        } else if (notification) {
            cJSON_Delete(rpc_result);
            outcome = mcp_rpc_send_no_content(request);
        } else {
            outcome = mcp_rpc_send_result_ex(request, rpc_result, id, &meta);
        }
    } else if (meta.mcp_2026) {
        // Unknown method in MCP 2026 mode -> HTTP 404 + JSON-RPC -32601
        outcome = mcp_rpc_send_error_ex(request, -32601, "Method not found",
                                        id, &meta, "404 Not Found", false);
    } else {
        // Legacy aliases: list_tools, call_tool
        if (strcmp(method->valuestring, "list_tools") == 0) {
            cJSON *rpc_result = mcp_tools_list(&meta);
            if (rpc_result == NULL) {
                outcome =
                    mcp_rpc_send_error(request, -32603, "Internal error", id);
            } else if (notification) {
                cJSON_Delete(rpc_result);
                outcome = mcp_rpc_send_no_content(request);
            } else {
                outcome =
                    mcp_rpc_send_result_ex(request, rpc_result, id, &meta);
            }
        } else if (strcmp(method->valuestring, "call_tool") == 0) {
            outcome = handle_tools_call(request, root, id, notification, &meta);
        } else {
            outcome = mcp_rpc_send_error(request, -32601, "Method not found",
                                         id);
        }
    }

    cJSON_Delete(root);
    return outcome;
}

int mcp_endpoint_register(httpd_handle_t server)
{
    if (server == NULL) return -1;
    mcp_transport_set(NULL);

    const httpd_uri_t route = {
        .uri = "/mcp",
        .method = HTTP_POST,
        .handler = mcp_handle_request,
        .user_ctx = NULL,
    };
    esp_err_t error = httpd_register_uri_handler(server, &route);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not register POST /mcp: %s", esp_err_to_name(error));
        return -1;
    }
    ESP_LOGI(TAG, "MCP JSON-RPC endpoint registered at POST /mcp");
    return 0;
}
