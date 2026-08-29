#ifndef MCP_CORE_H
#define MCP_CORE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MCP_TRANSPORT_HTTP = 0,
    MCP_TRANSPORT_WS,
} mcp_transport_kind_t;

typedef struct {
    mcp_transport_kind_t transport;
    bool has_protocol_version;
    bool has_method_metadata;
    bool has_name_metadata;
    bool authenticated;
    bool trusted_transport;
    char protocol_version[16];
    char method_metadata[32];
    char name_metadata[129];
} mcp_wire_context_t;

typedef struct {
    const char *http_status;
    bool close_connection;
} mcp_response_meta_t;

typedef struct mcp_responder mcp_responder_t;

typedef esp_err_t (*mcp_send_json_fn)(void *context, const char *json,
                                      size_t len,
                                      const mcp_response_meta_t *meta);
typedef esp_err_t (*mcp_send_none_fn)(void *context,
                                      const mcp_response_meta_t *meta);
typedef bool (*mcp_is_alive_fn)(void *context);
typedef esp_err_t (*mcp_responder_clone_fn)(const mcp_responder_t *source,
                                            mcp_responder_t *out);
typedef void (*mcp_release_fn)(void *context);

struct mcp_responder {
    void *context;
    mcp_send_json_fn send_json;
    mcp_send_none_fn send_none;
    mcp_is_alive_fn is_alive;
    mcp_responder_clone_fn clone;
    mcp_release_fn release;
};

// The responder is borrowed for synchronous handling. If a BLE command is
// queued, clone() creates an independently-owned responder that remains valid
// until its completion callback calls release() exactly once.
esp_err_t mcp_core_handle_json(const char *json, size_t json_len,
                               const mcp_wire_context_t *wire,
                               const mcp_responder_t *responder);

#ifdef __cplusplus
}
#endif

#endif // MCP_CORE_H
