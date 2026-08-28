#ifndef MCP_ENDPOINT_INTERNAL_H
#define MCP_ENDPOINT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cbor_codec.h"
#include "command_dispatcher.h"
#include "device_store.h"
#include "device_capabilities.h"
#include "mcp_tool_exposure.h"

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

#define MCP_PROTOCOL_VERSION_2026  "2026-07-28"
#define MCP_PROTOCOL_VERSION_2025  "2025-11-25"
#define MCP_SERVER_NAME            "esp32-ble-gateway"
#define MCP_SERVER_VERSION         "1.0.0"

#define MCP_PROTOCOL_VERSION_MAX   16
#define MCP_METHOD_MAX             32
#define MCP_NAME_DECODED_MAX       128

// ESP-IDF Kconfig bools expand to 0 or 1, usable in C expressions.
#ifndef CONFIG_MCP_COMPAT_2025
#define CONFIG_MCP_COMPAT_2025 0
#endif
#ifndef CONFIG_MCP_STRICT_ACCEPT_HEADER
#define CONFIG_MCP_STRICT_ACCEPT_HEADER 0
#endif

// Bounded retries when httpd_req_recv reports HTTPD_SOCK_ERR_TIMEOUT.
#define MCP_MAX_RECV_RETRIES 3

// JSON-RPC extended error codes.
// -32020..-32022: MCP protocol-defined (must NOT be reused for other purposes).
//   -32020: HeaderMismatch (Mcp-Method/Mcp-Name mismatch)
//   -32021: MissingRequiredClientCapability
//   -32022: UnsupportedProtocolVersion
// -31000: project-local gateway error (queue full) — outside MCP reserved range.
#define MCP_ERR_HEADER               -32020
#define MCP_ERR_MISSING_CAPABILITY   -32021
#define MCP_ERR_UNSUPPORTED_VERSION  -32022
#define MCP_ERR_GATEWAY_BUSY         -31000
#define MCP_ERR_COMMAND_DENIED       -32602
#define MCP_ERR_DEVICE_UNAVAILABLE   -32602
#define MCP_ERR_CAPABILITY_UNKNOWN   -32602

#define MCP_META_KEY_PROTOCOL_VERSION "io.modelcontextprotocol/protocolVersion"
#define MCP_META_KEY_CLIENT_INFO      "io.modelcontextprotocol/clientInfo"
#define MCP_META_KEY_CLIENT_CAPS      "io.modelcontextprotocol/clientCapabilities"
#define MCP_META_KEY_SERVER_INFO      "io.modelcontextprotocol/serverInfo"
#ifndef CONFIG_MCP_TOOLS_CACHE_TTL_MS
#define CONFIG_MCP_TOOLS_CACHE_TTL_MS 60000
#endif
#define MCP_TOOLS_CACHE_TTL_MS        CONFIG_MCP_TOOLS_CACHE_TTL_MS
#define MCP_TOOLS_CACHE_SCOPE         "private"

// ---------------------------------------------------------------------------
// Protocol era
// ---------------------------------------------------------------------------

typedef enum {
    MCP_ERA_UNKNOWN = 0,
    MCP_ERA_2025_11_25,
    MCP_ERA_2026_07_28,
} mcp_protocol_era_t;

// ---------------------------------------------------------------------------
// Request context — per-request, no session state
// ---------------------------------------------------------------------------

typedef struct {
    mcp_protocol_era_t era;
    bool initialize_request;
    bool notification;

    char protocol_version[MCP_PROTOCOL_VERSION_MAX];
    char mcp_method[MCP_METHOD_MAX];
    char mcp_name[MCP_NAME_DECODED_MAX + 1];

    bool has_protocol_header;
    bool has_method_header;
    bool has_name_header;
} mcp_request_context_t;

// ---------------------------------------------------------------------------
// Error detail with owned data
// ---------------------------------------------------------------------------

typedef struct {
    int rpc_code;
    const char *message;       // borrowed static/string-lifetime pointer
    const char *http_status;   // borrowed pointer or NULL
    cJSON *data;               // owned by detail until transferred
} mcp_rpc_error_detail_t;

void mcp_rpc_error_detail_init(mcp_rpc_error_detail_t *detail);
void mcp_rpc_error_detail_clear(mcp_rpc_error_detail_t *detail);

// Legacy alias kept for gradual migration; maps to mcp_request_context_t.
typedef mcp_request_context_t mcp_request_meta_t;

// ---------------------------------------------------------------------------
// Wire / RPC error (thin, for internal tool callers)
// ---------------------------------------------------------------------------

typedef struct {
    int code;
    const char *message;
} mcp_rpc_error_t;

// ---------------------------------------------------------------------------
// Transport hooks (test seam)
// ---------------------------------------------------------------------------

typedef struct {
    int (*recv)(httpd_req_t *req, char *buf, int buf_len);
    esp_err_t (*send)(httpd_req_t *req, const char *buf, size_t len);
    esp_err_t (*send_err)(httpd_req_t *req, int status, const char *message);
    esp_err_t (*set_type)(httpd_req_t *req, const char *type);
    esp_err_t (*set_status)(httpd_req_t *req, const char *status);
    esp_err_t (*set_hdr)(httpd_req_t *req, const char *field, const char *value);
    char *(*get_header)(httpd_req_t *req, const char *name);
    esp_err_t (*async_begin)(httpd_req_t *req, httpd_req_t **out);
    esp_err_t (*async_complete)(httpd_req_t *req);
} mcp_transport_t;

void mcp_transport_set(const mcp_transport_t *hooks);
const mcp_transport_t *mcp_transport_get(void);

// ---------------------------------------------------------------------------
// RPC envelopes (mcp_rpc.c)
// ---------------------------------------------------------------------------

esp_err_t mcp_rpc_send_error(httpd_req_t *request, int code,
                             const char *message, const cJSON *id);
esp_err_t mcp_rpc_send_result(httpd_req_t *request, cJSON *result,
                              const cJSON *id);

esp_err_t mcp_rpc_send_error_ex(httpd_req_t *request, int code,
                                const char *message, const cJSON *id,
                                const mcp_request_context_t *ctx,
                                const char *http_status, bool close_conn);
// Send error with structured error.data (ownership transferred, §15.1).
esp_err_t mcp_rpc_send_error_detail(httpd_req_t *request,
                                    mcp_rpc_error_detail_t *detail,
                                    const cJSON *id, bool close_conn);
esp_err_t mcp_rpc_send_result_ex(httpd_req_t *request, cJSON *result,
                                 const cJSON *id,
                                 const mcp_request_context_t *ctx);

// 202 Accepted for recognized notifications (empty body).
esp_err_t mcp_rpc_send_accepted(httpd_req_t *request);

// Helper: send a plain-text HTTP error with the exact status string.
esp_err_t mcp_http_send_plain_status(httpd_req_t *req, const char *status,
                                     const char *message, bool close_connection);

// ---------------------------------------------------------------------------
// Protocol detection (mcp_codec.c)
// ---------------------------------------------------------------------------

// Detect protocol era from HTTP headers + parsed JSON body.
// Returns 0 on success, JSON-RPC error code on failure.
int mcp_protocol_detect(httpd_req_t *req, const cJSON *root,
                        mcp_request_context_t *ctx,
                        mcp_rpc_error_detail_t *error);

// Validate era-specific request constraints.
int mcp_protocol_validate_request(const cJSON *root,
                                  const mcp_request_context_t *ctx,
                                  mcp_rpc_error_detail_t *error);

// Build InitializeResult for MCP 2025 compatibility.
cJSON *mcp_protocol_build_initialize_result(const cJSON *params,
                                            mcp_rpc_error_detail_t *error);

// Build -32022 error.data with supported + requested versions.
cJSON *mcp_protocol_build_unsupported_version_data(const char *requested);

// ---------------------------------------------------------------------------
// Wire codec (mcp_codec.c)
// ---------------------------------------------------------------------------

// Decode a potentially Base64-encoded Mcp-Name value.
// Caller must free() the returned string.
char *mcp_codec_decode_name(const char *raw_name);

// Build the server/discover result payload.
cJSON *mcp_codec_build_discovery(void);

// Add result._meta["io.modelcontextprotocol/serverInfo"] to a cJSON object.
bool mcp_result_add_server_info(cJSON *result);

// ---------------------------------------------------------------------------
// Tools formatting (mcp_tools.c, mcp_registry.c)
// ---------------------------------------------------------------------------

cJSON *mcp_tools_format_dispatch(const dispatch_result_t *result,
                                 const mcp_request_context_t *ctx,
                                 mcp_rpc_error_t *err);
cJSON *mcp_tools_tool_error(const char *text, const mcp_request_context_t *ctx,
                            mcp_rpc_error_t *err);

typedef enum {
    MCP_RESOLVE_OK = 0,
    MCP_RESOLVE_ALLOWLIST_DENIED,
    MCP_RESOLVE_INVALID,
} mcp_resolve_status_t;

typedef enum {
    MCP_POLICY_ALLOW = 0,
    MCP_POLICY_DENY_COMMAND,
    MCP_POLICY_DENY_DESTRUCTIVE,
    MCP_POLICY_DEVICE_UNAVAILABLE,
    MCP_POLICY_CAPABILITY_UNKNOWN,
} mcp_policy_result_t;

mcp_resolve_status_t mcp_tools_resolve(const cJSON *params,
                                       gw_message_t *msg,
                                       bool *is_device_command,
                                       char *denial_text, size_t denial_len,
                                       mcp_rpc_error_t *error);

cJSON *mcp_tools_execute(const gw_message_t *msg,
                         const mcp_request_context_t *ctx,
                         mcp_rpc_error_t *error);

cJSON *mcp_tools_list(const mcp_request_context_t *ctx);

bool mcp_device_command_allowed(const char *command);

// Registry (mcp_registry.c)
typedef struct {
    const char *name;
    const char *description;
    cJSON *(*input_schema)(void);
    bool read_only;
    bool destructive;
} mcp_tool_desc_t;

const mcp_tool_desc_t *mcp_registry_find(const char *name);
int mcp_registry_build_tools_list(cJSON *tools_array);

// Dynamic tool schema builder from device capability (mcp_registry.c).
cJSON *mcp_dynamic_tool_build_schema(const device_capability_t *cap);
cJSON *mcp_dynamic_tool_build_json(const mcp_tool_binding_t *binding);

// ---------------------------------------------------------------------------
// Policy (mcp_policy.c)
// ---------------------------------------------------------------------------

mcp_policy_result_t mcp_policy_check_device_command(const char *device_id,
                                                     const char *command);

// ---------------------------------------------------------------------------
// Auth / request gating (mcp_auth.c)
// ---------------------------------------------------------------------------

typedef enum {
    MCP_GATE_OK = 0,
    MCP_GATE_RATE_LIMITED,
    MCP_GATE_UNAUTHORIZED,
    MCP_GATE_FORBIDDEN_HOST,
    MCP_GATE_BAD_CONTENT_TYPE,
    MCP_GATE_BAD_ACCEPT,
} mcp_gate_status_t;

mcp_gate_status_t mcp_auth_gate(httpd_req_t *req);
void mcp_auth_reset_rate_limit(void);

// ---------------------------------------------------------------------------
// Endpoint core (mcp_endpoint.c)
// ---------------------------------------------------------------------------

esp_err_t mcp_handle_request(httpd_req_t *req);

#endif /* MCP_ENDPOINT_INTERNAL_H */
