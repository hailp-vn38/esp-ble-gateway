#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "mcp_core.h"
#include "mcp_endpoint.h"
#include "mcp_endpoint_internal.h"

#define MCP_MAX_REQUEST_LEN 4096

static const char *TAG = "mcp_endpoint";

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
    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0) return NULL;
    char *value = malloc(len + 1);
    if (value == NULL) return NULL;
    if (httpd_req_get_hdr_value_str(req, name, value, len + 1) != ESP_OK) {
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

// HTTP-only test seam. Production WebSocket code never reads or changes it.
static mcp_transport_t s_transport;

void mcp_transport_set(const mcp_transport_t *hooks)
{
    s_transport = hooks != NULL ? *hooks : s_default_transport;
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

static const char *gate_status_string(mcp_gate_status_t gate)
{
    switch (gate) {
    case MCP_GATE_RATE_LIMITED: return "429 Too Many Requests";
    case MCP_GATE_UNAUTHORIZED: return "401 Unauthorized";
    case MCP_GATE_FORBIDDEN_HOST: return "403 Forbidden";
    case MCP_GATE_BAD_CONTENT_TYPE: return "415 Unsupported Media Type";
    case MCP_GATE_BAD_ACCEPT: return "406 Not Acceptable";
    case MCP_GATE_OK: return NULL;
    }
    return "400 Bad Request";
}

static esp_err_t send_gate_error(httpd_req_t *request, mcp_gate_status_t gate)
{
    const char *status = gate_status_string(gate);
    if (status == NULL) return ESP_OK;
    s_transport.set_status(request, status);
    s_transport.set_hdr(request, "Connection", "close");
    if (gate == MCP_GATE_UNAUTHORIZED) {
        s_transport.set_hdr(request, "WWW-Authenticate", "Bearer");
    }
    return s_transport.send(request, status, HTTPD_RESP_USE_STRLEN);
}

static void populate_header(httpd_req_t *request, const char *name, char *out,
                            size_t out_size, bool *present)
{
    char *value = s_transport.get_header(request, name);
    *present = value != NULL;
    if (value != NULL) {
        strlcpy(out, value, out_size);
        free(value);
    }
}

static esp_err_t http_send_json(void *context, const char *json, size_t len,
                                const mcp_response_meta_t *meta)
{
    httpd_req_t *request = context;
    if (meta != NULL && meta->http_status != NULL) {
        s_transport.set_status(request, meta->http_status);
    }
    if (meta != NULL && meta->close_connection) {
        s_transport.set_hdr(request, "Connection", "close");
    }
    s_transport.set_type(request, "application/json");
    return s_transport.send(request, json, len);
}

static esp_err_t http_send_none(void *context,
                                const mcp_response_meta_t *meta)
{
    httpd_req_t *request = context;
    if (meta != NULL && meta->http_status != NULL) {
        s_transport.set_status(request, meta->http_status);
    }
    if (meta != NULL && meta->close_connection) {
        s_transport.set_hdr(request, "Connection", "close");
    }
    return s_transport.send(request, NULL, 0);
}

static bool http_is_alive(void *context)
{
    return context != NULL;
}

static esp_err_t http_responder_clone(const mcp_responder_t *source,
                                      mcp_responder_t *out)
{
    httpd_req_t *async_request = NULL;
    esp_err_t result =
        s_transport.async_begin(source->context, &async_request);
    if (result != ESP_OK) return result;
    *out = *source;
    out->context = async_request;
    return ESP_OK;
}

static void http_responder_release(void *context)
{
    if (s_transport.async_complete(context) != ESP_OK) {
        ESP_LOGD(TAG, "async_complete failed (client gone?)");
    }
}

static mcp_responder_t make_http_responder(httpd_req_t *request)
{
    const mcp_responder_t responder = {
        .context = request,
        .send_json = http_send_json,
        .send_none = http_send_none,
        .is_alive = http_is_alive,
        .clone = http_responder_clone,
        .release = http_responder_release,
    };
    return responder;
}

esp_err_t mcp_handle_request(httpd_req_t *request)
{
    mcp_gate_status_t gate = mcp_auth_gate(request);
    if (gate != MCP_GATE_OK) return send_gate_error(request, gate);

    char *body = NULL;
    mcp_recv_status_t received = receive_body(request, &body);
    if (received != MCP_RECV_OK) {
        const char *message = "Parse error";
        int code = -32700;
        const char *status = NULL;
        if (received == MCP_RECV_ERR_SIZE) {
            message = "Invalid Request: body exceeds limit";
            code = -32600;
            status = "413 Content Too Large";
        } else if (received == MCP_RECV_ERR_MEM) {
            message = "Internal error";
            code = -32603;
        }
        return mcp_rpc_send_error_ex(request, code, message, NULL, NULL,
                                     status,
                                     received == MCP_RECV_ERR_SIZE);
    }

    mcp_wire_context_t wire = {
        .transport = MCP_TRANSPORT_HTTP,
        .authenticated = true,
        .trusted_transport = false,
    };
    populate_header(request, "MCP-Protocol-Version", wire.protocol_version,
                    sizeof(wire.protocol_version),
                    &wire.has_protocol_version);
    populate_header(request, "Mcp-Method", wire.method_metadata,
                    sizeof(wire.method_metadata), &wire.has_method_metadata);
    populate_header(request, "Mcp-Name", wire.name_metadata,
                    sizeof(wire.name_metadata), &wire.has_name_metadata);

    mcp_responder_t responder = make_http_responder(request);
    esp_err_t outcome =
        mcp_core_handle_json(body, strlen(body), &wire, &responder);
    free(body);
    if (outcome == ESP_ERR_NO_MEM) {
        return s_transport.send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                    "Out of memory");
    }
    return outcome;
}

static esp_err_t handle_non_post_mcp(httpd_req_t *request)
{
    s_transport.set_status(request, "405 Method Not Allowed");
    s_transport.set_hdr(request, "Allow", "POST");
    s_transport.set_hdr(request, "Connection", "close");
    return s_transport.send(request, NULL, 0);
}

int mcp_endpoint_register(httpd_handle_t server)
{
    if (server == NULL) return -1;
    mcp_transport_set(NULL);
    const httpd_uri_t routes[] = {
        {.uri = "/mcp", .method = HTTP_POST, .handler = mcp_handle_request},
        {.uri = "/mcp", .method = HTTP_GET, .handler = handle_non_post_mcp},
        {.uri = "/mcp", .method = HTTP_DELETE, .handler = handle_non_post_mcp},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t result = httpd_register_uri_handler(server, &routes[i]);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Could not register %s: %s", routes[i].uri,
                     esp_err_to_name(result));
            return -1;
        }
    }
    ESP_LOGI(TAG, "MCP endpoint registered: POST/GET/DELETE /mcp");
    return 0;
}
