#ifndef MCP_WS_BRIDGE_H
#define MCP_WS_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_WS_ENDPOINT_MAX_LEN 768
#define MCP_WS_ENDPOINT_DISPLAY_MAX_LEN 192

typedef enum {
    MCP_WS_DISABLED = 0,
    MCP_WS_WAIT_NETWORK,
    MCP_WS_CONNECTING,
    MCP_WS_HANDSHAKING,
    MCP_WS_READY,
    MCP_WS_BACKOFF,
    MCP_WS_ERROR,
} mcp_ws_state_t;

typedef struct {
    bool enabled;
    char endpoint[MCP_WS_ENDPOINT_MAX_LEN];
} mcp_ws_config_t;

typedef struct {
    bool enabled;
    bool endpoint_configured;
    char endpoint_display[MCP_WS_ENDPOINT_DISPLAY_MAX_LEN];
} mcp_ws_public_config_t;

typedef struct {
    bool enabled;
    bool endpoint_configured;
    mcp_ws_state_t state;
    uint32_t generation;
    uint32_t retry_count;
    int last_error;
    int last_http_status;
    int last_ws_close_code;
    char negotiated_protocol_version[16];
} mcp_ws_status_t;

esp_err_t mcp_ws_bridge_init(void);
esp_err_t mcp_ws_bridge_start(void);
esp_err_t mcp_ws_bridge_stop(void);
esp_err_t mcp_ws_bridge_reload(void);
esp_err_t mcp_ws_bridge_get_status(mcp_ws_status_t *out);

esp_err_t mcp_ws_bridge_config_set(const mcp_ws_config_t *config);
esp_err_t mcp_ws_bridge_config_update(bool has_enabled, bool enabled,
                                      bool has_endpoint,
                                      const char *endpoint);
esp_err_t mcp_ws_bridge_config_get_public(mcp_ws_public_config_t *out);
esp_err_t mcp_ws_bridge_config_clear(void);

const char *mcp_ws_bridge_state_name(mcp_ws_state_t state);

#ifdef __cplusplus
}
#endif

#endif // MCP_WS_BRIDGE_H
