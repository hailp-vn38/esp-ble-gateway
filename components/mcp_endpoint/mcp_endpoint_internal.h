#ifndef MCP_ENDPOINT_INTERNAL_H
#define MCP_ENDPOINT_INTERNAL_H

#include "cJSON.h"
#include "esp_http_server.h"

typedef struct {
    int code;
    const char *message;
} mcp_rpc_error_t;

esp_err_t mcp_rpc_send_error(httpd_req_t *request, int code,
                             const char *message, const cJSON *id);
esp_err_t mcp_rpc_send_result(httpd_req_t *request, cJSON *result,
                              const cJSON *id);

cJSON *mcp_tools_list(void);
cJSON *mcp_tools_call(const cJSON *params, mcp_rpc_error_t *error);

#endif /* MCP_ENDPOINT_INTERNAL_H */
