#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "mcp_endpoint.h"
#include "mcp_endpoint_internal.h"

#define MCP_MAX_REQUEST_LEN 4096

static const char *TAG = "mcp_endpoint";

static char *receive_body(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > MCP_MAX_REQUEST_LEN) {
        return NULL;
    }
    char *body = malloc((size_t)request->content_len + 1);
    if (body == NULL) return NULL;

    size_t received = 0;
    while (received < (size_t)request->content_len) {
        int chunk = httpd_req_recv(request, body + received,
                                   request->content_len - (int)received);
        if (chunk == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (chunk <= 0) {
            free(body);
            return NULL;
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';
    return body;
}

static esp_err_t mcp_post_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > MCP_MAX_REQUEST_LEN) {
        return mcp_rpc_send_error(request, -32600, "Invalid Request", NULL);
    }
    char *body = receive_body(request);
    if (body == NULL) {
        return mcp_rpc_send_error(request, -32700, "Parse error", NULL);
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return mcp_rpc_send_error(request, -32700, "Parse error", NULL);
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "jsonrpc");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    bool valid_id = id == NULL || cJSON_IsString(id) || cJSON_IsNumber(id) ||
                    cJSON_IsNull(id);
    if (!cJSON_IsString(version) || version->valuestring == NULL ||
        strcmp(version->valuestring, "2.0") != 0 || !cJSON_IsString(method) ||
        method->valuestring == NULL || method->valuestring[0] == '\0' || !valid_id) {
        esp_err_t result = mcp_rpc_send_error(
            request, -32600, "Invalid Request", id);
        cJSON_Delete(root);
        return result;
    }

    bool notification = id == NULL;
    cJSON *rpc_result = NULL;
    mcp_rpc_error_t rpc_error = {0};
    if (strcmp(method->valuestring, "list_tools") == 0 ||
        strcmp(method->valuestring, "tools/list") == 0) {
        rpc_result = mcp_tools_list();
        if (rpc_result == NULL) {
            rpc_error = (mcp_rpc_error_t){-32603, "Internal error"};
        }
    } else if (strcmp(method->valuestring, "call_tool") == 0 ||
               strcmp(method->valuestring, "tools/call") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        rpc_result = mcp_tools_call(params, &rpc_error);
    } else {
        rpc_error = (mcp_rpc_error_t){-32601, "Method not found"};
    }

    if (notification) {
        cJSON_Delete(rpc_result);
        cJSON_Delete(root);
        httpd_resp_set_status(request, "204 No Content");
        return httpd_resp_send(request, NULL, 0);
    }

    esp_err_t result;
    if (rpc_error.code != 0) {
        cJSON_Delete(rpc_result);
        result = mcp_rpc_send_error(
            request, rpc_error.code, rpc_error.message, id);
    } else {
        result = mcp_rpc_send_result(request, rpc_result, id);
    }
    cJSON_Delete(root);
    return result;
}

int mcp_endpoint_register(httpd_handle_t server)
{
    if (server == NULL) return -1;
    const httpd_uri_t route = {
        .uri = "/mcp",
        .method = HTTP_POST,
        .handler = mcp_post_handler,
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
