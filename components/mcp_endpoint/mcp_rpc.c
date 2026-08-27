#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"

#include "mcp_endpoint_internal.h"

// ---------------------------------------------------------------------------
// Error detail helpers (§15.1)
// ---------------------------------------------------------------------------

void mcp_rpc_error_detail_init(mcp_rpc_error_detail_t *detail)
{
    detail->rpc_code = 0;
    detail->message = NULL;
    detail->http_status = NULL;
    detail->data = NULL;
}

void mcp_rpc_error_detail_clear(mcp_rpc_error_detail_t *detail)
{
    if (detail->data != NULL) {
        cJSON_Delete(detail->data);
        detail->data = NULL;
    }
}

// ---------------------------------------------------------------------------
// ID handling
// ---------------------------------------------------------------------------

static bool mcp_rpc_valid_request_id(const cJSON *id)
{
    return id != NULL && (cJSON_IsString(id) || cJSON_IsNumber(id));
}

static cJSON *duplicate_id(const cJSON *id)
{
    if (id == NULL) return cJSON_CreateNull();
    if (!mcp_rpc_valid_request_id(id)) return cJSON_CreateNull();
    return cJSON_Duplicate(id, true);
}

// ---------------------------------------------------------------------------
// JSON send
// ---------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *request, cJSON *response)
{
    const mcp_transport_t *io = mcp_transport_get();
    if (response == NULL) {
        io->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                     "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    char *body = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (body == NULL) {
        io->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                     "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    io->set_type(request, "application/json");
    esp_err_t error = io->send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return error;
}

// ---------------------------------------------------------------------------
// Envelope builder
// ---------------------------------------------------------------------------

static cJSON *build_envelope(cJSON *response_id)
{
    cJSON *response = cJSON_CreateObject();
    if (response == NULL) return NULL;
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id", response_id);
    return response;
}

// ---------------------------------------------------------------------------
// Error builder with data ownership transfer (§15.1)
// ---------------------------------------------------------------------------

static esp_err_t finish_error(httpd_req_t *request, cJSON *envelope,
                              int code, const char *message,
                              const char *http_status,
                              mcp_rpc_error_detail_t *detail,
                              bool close_conn)
{
    cJSON *error = cJSON_CreateObject();
    if (error == NULL) {
        cJSON_Delete(envelope);
        if (detail != NULL) mcp_rpc_error_detail_clear(detail);
        mcp_transport_get()->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);

    // Transfer ownership of detail->data into error object (§15.1)
    if (detail != NULL && detail->data != NULL) {
        cJSON_AddItemToObject(error, "data", detail->data);
        detail->data = NULL;  // ownership transferred
    }

    cJSON_AddItemToObject(envelope, "error", error);

    if (close_conn) {
        mcp_transport_get()->set_hdr(request, "Connection", "close");
    }
    if (http_status != NULL) {
        mcp_transport_get()->set_status(request, http_status);
    }

    return send_json(request, envelope);
}

static esp_err_t finish_result(httpd_req_t *request, cJSON *envelope,
                               cJSON *result)
{
    if (result == NULL || !cJSON_AddItemToObject(envelope, "result", result)) {
        cJSON_Delete(result);
        cJSON_Delete(envelope);
        mcp_transport_get()->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    return send_json(request, envelope);
}

// ---------------------------------------------------------------------------
// Public API — simple variants (no meta/context)
// ---------------------------------------------------------------------------

esp_err_t mcp_rpc_send_error(httpd_req_t *request, int code,
                             const char *message, const cJSON *id)
{
    return mcp_rpc_send_error_ex(request, code, message, id, NULL, NULL, false);
}

esp_err_t mcp_rpc_send_result(httpd_req_t *request, cJSON *result,
                              const cJSON *id)
{
    return mcp_rpc_send_result_ex(request, result, id, NULL);
}

// ---------------------------------------------------------------------------
// Public API — full-control variants
// ---------------------------------------------------------------------------

esp_err_t mcp_rpc_send_error_ex(httpd_req_t *request, int code,
                                const char *message, const cJSON *id,
                                const mcp_request_context_t *ctx,
                                const char *http_status, bool close_conn)
{
    cJSON *response_id = duplicate_id(id);
    cJSON *envelope = response_id != NULL ? build_envelope(response_id) : NULL;
    if (envelope == NULL) {
        cJSON_Delete(response_id);
        mcp_transport_get()->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    // No auto-add _meta on error envelopes (§12.4 / §14.4).
    // serverInfo is only added by result builders.
    return finish_error(request, envelope, code, message, http_status, NULL,
                        close_conn);
}

// Overload with detail for structured error data (§15.1).
esp_err_t mcp_rpc_send_error_detail(httpd_req_t *request,
                                    mcp_rpc_error_detail_t *detail,
                                    const cJSON *id,
                                    bool close_conn)
{
    if (detail == NULL || detail->rpc_code == 0) {
        return mcp_rpc_send_error(request, -32603, "Internal error", id);
    }
    cJSON *response_id = duplicate_id(id);
    cJSON *envelope = response_id != NULL ? build_envelope(response_id) : NULL;
    if (envelope == NULL) {
        cJSON_Delete(response_id);
        mcp_rpc_error_detail_clear(detail);
        mcp_transport_get()->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    return finish_error(request, envelope, detail->rpc_code, detail->message,
                        detail->http_status, detail, close_conn);
}

esp_err_t mcp_rpc_send_result_ex(httpd_req_t *request, cJSON *result,
                                 const cJSON *id,
                                 const mcp_request_context_t *ctx)
{
    cJSON *response_id = duplicate_id(id);
    cJSON *envelope = response_id != NULL ? build_envelope(response_id) : NULL;
    if (envelope == NULL) {
        cJSON_Delete(response_id);
        cJSON_Delete(result);
        mcp_transport_get()->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    // No auto-add _meta on envelopes. Result builders (mcp_tools) add
    // serverInfo to result._meta themselves (§12.4).
    return finish_result(request, envelope, result);
}

// ---------------------------------------------------------------------------
// 202 Accepted — for recognized notifications (§12.1)
// ---------------------------------------------------------------------------

esp_err_t mcp_rpc_send_accepted(httpd_req_t *request)
{
    const mcp_transport_t *io = mcp_transport_get();
    io->set_status(request, "202 Accepted");
    return io->send(request, NULL, 0);
}

// ---------------------------------------------------------------------------
// Plain-text HTTP error helper (§12.5)
// ---------------------------------------------------------------------------

esp_err_t mcp_http_send_plain_status(httpd_req_t *req, const char *status,
                                     const char *message,
                                     bool close_connection)
{
    const mcp_transport_t *io = mcp_transport_get();
    io->set_status(req, status);
    io->set_type(req, "text/plain");
    if (close_connection) {
        io->set_hdr(req, "Connection", "close");
    }
    return io->send(req, message, HTTPD_RESP_USE_STRLEN);
}
