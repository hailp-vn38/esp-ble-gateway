#include <stdbool.h>

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
    if (response == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    char *body = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (body == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(request, "application/json");
    esp_err_t error = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return error;
}

esp_err_t mcp_rpc_send_error(httpd_req_t *request, int code,
                             const char *message, const cJSON *id)
{
    cJSON *response = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    cJSON *response_id = duplicate_id(id);
    if (response == NULL || error == NULL || response_id == NULL) {
        cJSON_Delete(response);
        cJSON_Delete(error);
        cJSON_Delete(response_id);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id", response_id);
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(response, "error", error);
    return send_json(request, response);
}

esp_err_t mcp_rpc_send_result(httpd_req_t *request, cJSON *result,
                              const cJSON *id)
{
    cJSON *response = cJSON_CreateObject();
    cJSON *response_id = duplicate_id(id);
    if (response == NULL || response_id == NULL || result == NULL) {
        cJSON_Delete(response);
        cJSON_Delete(response_id);
        cJSON_Delete(result);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id", response_id);
    cJSON_AddItemToObject(response, "result", result);
    return send_json(request, response);
}
