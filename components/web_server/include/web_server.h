#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_http_server.h"

// Tra ve httpd_handle_t da start (de mcp_endpoint dang ky them route /mcp
// vao CUNG mot server), hoac NULL neu loi.
httpd_handle_t web_server_start(void);

#endif // WEB_SERVER_H
