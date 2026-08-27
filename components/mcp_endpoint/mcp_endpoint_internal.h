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

// ---------------------------------------------------------------------------
// Shared error / wire constants
// ---------------------------------------------------------------------------

#define MCP_PROTOCOL_VERSION_2026 "2026-07-28"
#define MCP_SERVER_NAME           "esp32-ble-gateway"
#define MCP_SERVER_VERSION        "1.0.0"

// Bounded retries when httpd_req_recv reports HTTPD_SOCK_ERR_TIMEOUT.
#define MCP_MAX_RECV_RETRIES 3

// JSON-RPC extended error codes.
// -32020..-32022: MCP protocol-defined (must NOT be reused for other purposes).
//   -32020: HeaderMismatch (Mcp-Method/Mcp-Name mismatch)
//   -32021: MissingRequiredClientCapability
//   -32022: UnsupportedProtocolVersion
// -32000..-32019: project-local gateway errors.
//   -32000: Gateway busy (queue full)
//   -32001: Device unavailable
//   -32002: Command denied
//   -32003: Capability unavailable
// Auth/security failures use HTTP 401/403, NOT JSON-RPC error codes.
#define MCP_ERR_HEADER               -32020
#define MCP_ERR_MISSING_CAPABILITY   -32021
#define MCP_ERR_UNSUPPORTED_VERSION  -32022
#define MCP_ERR_GATEWAY_BUSY         -32000
#define MCP_ERR_DEVICE_UNAVAILABLE   -32001
#define MCP_ERR_COMMAND_DENIED       -32002
#define MCP_ERR_CAPABILITY_UNKNOWN   -32003

#define MCP_META_KEY_PROTOCOL_VERSION "io.modelcontextprotocol/protocolVersion"
#define MCP_META_KEY_CLIENT_INFO      "io.modelcontextprotocol/clientInfo"
#define MCP_META_KEY_CLIENT_CAPS      "io.modelcontextprotocol/clientCapabilities"
#define MCP_META_KEY_SERVER_INFO      "io.modelcontextprotocol/serverInfo"
#define MCP_TOOLS_CACHE_TTL_MS        60000
#define MCP_TOOLS_CACHE_SCOPE         "private"

typedef struct {
    int code;
    const char *message;
} mcp_rpc_error_t;

// Per-request wire metadata extracted from HTTP headers by mcp_codec.
// Only stores routing-relevant data; full _meta tree is NOT kept.
typedef struct {
    bool mcp_2026;          // request opted into the 2026-07-28 wire format
    char mcp_method[32];    // from Mcp-Method header
    char mcp_name[64];      // from Mcp-Name header (may be Base64-encoded)
    bool has_name;          // true when Mcp-Name header was present
} mcp_request_meta_t;

// ---------------------------------------------------------------------------
// Transport hooks (test seam)
//
// Production defaults call the real esp_http_server API. Unit tests inject
// mocks via mcp_transport_set() instead of weak-symbol overrides so that the
// real component stays linkable (same pattern as device_command_set_hooks()).
// ---------------------------------------------------------------------------

typedef struct {
    // Returns bytes read (>0), 0 on peer close, or negative socket error
    // including HTTPD_SOCK_ERR_TIMEOUT (mirrors httpd_req_recv semantics).
    int (*recv)(httpd_req_t *req, char *buf, int buf_len);
    esp_err_t (*send)(httpd_req_t *req, const char *buf, size_t len);
    esp_err_t (*send_err)(httpd_req_t *req, int status, const char *message);
    esp_err_t (*set_type)(httpd_req_t *req, const char *type);
    esp_err_t (*set_status)(httpd_req_t *req, const char *status);
    esp_err_t (*set_hdr)(httpd_req_t *req, const char *field, const char *value);
    // Returns a freshly malloc'd header value or NULL when absent.
    char *(*get_header)(httpd_req_t *req, const char *name);
    esp_err_t (*async_begin)(httpd_req_t *req, httpd_req_t **out);
    esp_err_t (*async_complete)(httpd_req_t *req);
} mcp_transport_t;

void mcp_transport_set(const mcp_transport_t *hooks);
// Borrowed pointer to the active hook set (never NULL after
// mcp_transport_set(NULL) at registration time).
const mcp_transport_t *mcp_transport_get(void);

// ---------------------------------------------------------------------------
// RPC envelopes (mcp_rpc.c)
// ---------------------------------------------------------------------------

esp_err_t mcp_rpc_send_error(httpd_req_t *request, int code,
                             const char *message, const cJSON *id);
esp_err_t mcp_rpc_send_result(httpd_req_t *request, cJSON *result,
                              const cJSON *id);

// Full-control variants: choose legacy vs 2026 envelope and override the
// HTTP status line (NULL keeps 200). `close_conn` adds Connection: close for
// paths that did not drain the request body.
esp_err_t mcp_rpc_send_error_ex(httpd_req_t *request, int code,
                                const char *message, const cJSON *id,
                                const mcp_request_meta_t *meta,
                                const char *http_status, bool close_conn);
esp_err_t mcp_rpc_send_result_ex(httpd_req_t *request, cJSON *result,
                                 const cJSON *id, const mcp_request_meta_t *meta);
esp_err_t mcp_rpc_send_no_content(httpd_req_t *request);

// Builds (does not send) the tool outcome payload for one dispatch result in
// the wire format selected by meta. Returns NULL with err set on OOM.
cJSON *mcp_tools_format_dispatch(const dispatch_result_t *result,
                                 const mcp_request_meta_t *meta,
                                 mcp_rpc_error_t *err);
// Tool-level failure payload (isError true / success false), never OOM-safe
// to skip: falls back to NULL + err on OOM like every other builder.
cJSON *mcp_tools_tool_error(const char *text, const mcp_request_meta_t *meta,
                            mcp_rpc_error_t *err);

// ---------------------------------------------------------------------------
// Tools + registry (mcp_tools.c, mcp_registry.c)
// ---------------------------------------------------------------------------

typedef enum {
    MCP_RESOLVE_OK = 0,
    MCP_RESOLVE_ALLOWLIST_DENIED,
    MCP_RESOLVE_INVALID,
} mcp_resolve_status_t;

// Policy evaluation result per spec §16.
typedef enum {
    MCP_POLICY_ALLOW = 0,
    MCP_POLICY_DENY_COMMAND,        // command not in allowlist
    MCP_POLICY_DENY_DESTRUCTIVE,    // destructive command denied in control profile
    MCP_POLICY_DEVICE_UNAVAILABLE,  // device not in store
    MCP_POLICY_CAPABILITY_UNKNOWN,  // capabilities not ready / command not advertised
} mcp_policy_result_t;

// Validates params and normalizes them into a gw_message_t without touching
// the dispatcher. On MCP_RESOLVE_ALLOWLIST_DENIED, denial_text receives a
// short human-readable reason.
mcp_resolve_status_t mcp_tools_resolve(const cJSON *params,
                                       gw_message_t *msg,
                                       bool *is_device_command,
                                       char *denial_text, size_t denial_len,
                                       mcp_rpc_error_t *error);

// Takes the dispatch lock, runs command_dispatcher_handle and formats the
// result per wire mode. The caller owns the returned object.
cJSON *mcp_tools_execute(const gw_message_t *msg,
                         const mcp_request_meta_t *meta,
                         mcp_rpc_error_t *error);

cJSON *mcp_tools_list(const mcp_request_meta_t *meta);

bool mcp_device_command_allowed(const char *command);

// mcp_registry.c: strict tool table (single source of truth for tools/list).
typedef struct {
    const char *name;
    const char *description;
    cJSON *(*input_schema)(void);
    bool read_only;
    bool destructive;
} mcp_tool_desc_t;

const mcp_tool_desc_t *mcp_registry_find(const char *name);
int mcp_registry_build_tools_list(cJSON *tools_array, cJSON *names_array);

// ---------------------------------------------------------------------------
// MCP policy (mcp_policy.c)
// ---------------------------------------------------------------------------

// Evaluate whether a device command is allowed through MCP control profile.
// Sequence: device exists -> capabilities ready -> command advertised ->
// allowlist check -> destructive guard.
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
} mcp_gate_status_t;

// Header-only checks, run before any body byte is read. On anything but
// MCP_GATE_OK the endpoint must answer with an HTTP error and Connection:
// close (the body is left unread on the socket).
mcp_gate_status_t mcp_auth_gate(httpd_req_t *req);

// Token resolution order: NVS override (namespace "mcp", key "token") ->
// CONFIG_MCP_AUTH_TOKEN. Empty token keeps dev mode (no authentication).
// Rate-limit state is reset by mcp_auth_reset_rate_limit() (used by tests).
void mcp_auth_reset_rate_limit(void);

// ---------------------------------------------------------------------------
// Endpoint core (mcp_endpoint.c)
// ---------------------------------------------------------------------------

// The POST /mcp handler body, factored out of esp_http_server so unit tests
// can drive a full request/response cycle through mocked transport hooks.
esp_err_t mcp_handle_request(httpd_req_t *req);

// ---------------------------------------------------------------------------
// Wire codec (mcp_codec.c)
// ---------------------------------------------------------------------------

// Reads CONFIG_MCP_LEGACY_MODE plus the NVS runtime override (namespace "mcp",
// key "legacy"). Missing NVS entry or NVS errors fall back to Kconfig.
bool mcp_codec_legacy_enabled(void);
// Writes the NVS runtime override (-1 deletes it, restoring Kconfig default).
int mcp_codec_set_legacy_override(int value);

// Fills meta from request headers. Returns:
//   0          meta ready
//   MCP_ERR_VERSION  header missing while legacy disabled, or unsupported value
int mcp_codec_parse_meta(httpd_req_t *req, mcp_request_meta_t *meta);

// Builds the server/discover result payload (target MCP 2026-07-28 shape).
cJSON *mcp_codec_build_discovery(void);

// ---------------------------------------------------------------------------
// Protocol validation (mcp_codec.c)
// ---------------------------------------------------------------------------

// Validate required _meta fields in the parsed JSON body.
// Returns 0 on success, JSON-RPC error code on failure.
int mcp_protocol_validate_meta(const cJSON *root);

// Validate header/body consistency:
//   - Mcp-Method must exist and match body "method"
//   - Mcp-Name must exist for tools/call and match params.name
// Returns 0 on success, -32020 (HeaderMismatch) on failure.
int mcp_protocol_validate_headers(const cJSON *root,
                                  const mcp_request_meta_t *meta);

// Decode a potentially Base64-encoded Mcp-Name value.
// If the value is plain header-safe text, returns it directly.
// If it starts with the Base64 sentinel prefix, decodes and returns
// the decoded name. Returns NULL on decode error or buffer overflow.
// Caller must free() the returned string.
char *mcp_protocol_decode_name(const char *raw_name);

// Add result._meta["io.modelcontextprotocol/serverInfo"] to a cJSON object.
// Returns false on OOM.
bool mcp_result_add_server_info(cJSON *result);

#endif /* MCP_ENDPOINT_INTERNAL_H */
