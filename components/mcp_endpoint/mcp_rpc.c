#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"

#include "mcp_endpoint_internal.h"

static cJSON *duplicate_id(const cJSON *id)
{
    if (id == NULL || (!cJSON_IsString(id) && !cJSON_IsNumber(id) &&
                       !cJSON_IsNull(id))) {
        return cJSON_CreateNull();
    }
    return cJSON_Duplicate(id, true);
}

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

static cJSON *build_envelope(cJSON *response_id)
{
    cJSON *response = cJSON_CreateObject();
    if (response == NULL) return NULL;
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id", response_id);
    return response;
}

// Attaches the 2026-07-28 _meta block (protocol version + server identity).
static bool add_meta(cJSON *response)
{
    cJSON *meta = cJSON_CreateObject();
    cJSON *server = cJSON_CreateObject();
    if (meta == NULL || server == NULL) {
        cJSON_Delete(meta);
        cJSON_Delete(server);
        return false;
    }
    cJSON_AddStringToObject(meta, MCP_META_KEY_PROTOCOL_VERSION,
                            MCP_PROTOCOL_VERSION_2026);
    cJSON_AddStringToObject(server, "name", MCP_SERVER_NAME);
    cJSON_AddStringToObject(server, "version", MCP_SERVER_VERSION);
    cJSON_AddItemToObject(meta, MCP_META_KEY_SERVER, server);
    cJSON_AddItemToObject(response, "_meta", meta);
    return true;
}

static esp_err_t finish_error(httpd_req_t *request, cJSON *envelope,
                              int code, const char *message,
                              const char *http_status, const mcp_request_meta_t *meta,
                              bool close_conn)
{
    cJSON *error = cJSON_CreateObject();
    if (error == NULL) {
        cJSON_Delete(envelope);
        mcp_transport_get()->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(envelope, "error", error);
    if (close_conn) {
        // The request body was not drained; a persistent connection would
        // desync on the next pipelined request.
        mcp_transport_get()->set_hdr(request, "Connection", "close");
    }
    if (http_status != NULL) {
        mcp_transport_get()->set_status(request, http_status);
    }
    if (meta != NULL && meta->mcp_2026 && !add_meta(envelope)) {
        cJSON_Delete(envelope);
        envelope = NULL;
    }
    if (envelope == NULL) {
        mcp_transport_get()->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    return send_json(request, envelope);
}

static esp_err_t finish_result(httpd_req_t *request, cJSON *envelope,
                               cJSON *result, const mcp_request_meta_t *meta)
{
    if (result == NULL || !cJSON_AddItemToObject(envelope, "result", result)) {
        cJSON_Delete(result);
        cJSON_Delete(envelope);
        mcp_transport_get()->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    result = NULL;
    if (meta != NULL && meta->mcp_2026 && !add_meta(envelope)) {
        cJSON_Delete(envelope);
        envelope = NULL;
    }
    if (envelope == NULL) {
        mcp_transport_get()->send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    return send_json(request, envelope);
}

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

esp_err_t mcp_rpc_send_error_ex(httpd_req_t *request, int code,
                                const char *message, const cJSON *id,
                                const mcp_request_meta_t *meta,
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
    return finish_error(request, envelope, code, message, http_status, meta,
                        close_conn);
}

esp_err_t mcp_rpc_send_result_ex(httpd_req_t *request, cJSON *result,
                                 const cJSON *id,
                                 const mcp_request_meta_t *meta)
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
    return finish_result(request, envelope, result, meta);
}

esp_err_t mcp_rpc_send_no_content(httpd_req_t *request)
{
    mcp_transport_get()->set_status(request, "204 No Content");
    return mcp_transport_get()->send(request, NULL, 0);
}
