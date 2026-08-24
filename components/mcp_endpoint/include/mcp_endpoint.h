#ifndef MCP_ENDPOINT_H
#define MCP_ENDPOINT_H

#include "esp_http_server.h"

// Dang ky route POST /mcp vao HTTP server da co san (dung chung voi web_server)
int mcp_endpoint_register(httpd_handle_t server);

#endif // MCP_ENDPOINT_H
