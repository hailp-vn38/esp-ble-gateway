// Threat model (LAN control plane):
//
// - Bearer token travels over plaintext HTTP: it can be sniffed and replayed
//   by anyone on the same LAN segment. The token is a convenience gate for a
//   trusted home/office network, NOT a substitute for TLS. Never expose
//   POST /mcp beyond the LAN.
// - Host/Origin validation exists to blunt DNS-rebinding from browsers; the
//   Host header is attacker-controlled on direct requests and must never be
//   treated as authentication.
// - An empty CONFIG_MCP_AUTH_TOKEN means dev mode: requests are allowed but
//   warned once at startup. Production deployments must set a token.
// - Tokens are compared in constant time and never logged.

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "sdkconfig.h"

#include "mcp_endpoint_internal.h"

#define MCP_RATE_CAPACITY 10
#define MCP_RATE_WINDOW_US 1000000LL
#define MCP_NVS_NAMESPACE "mcp"

static const char *TAG = "mcp_auth";

static bool s_dev_mode_warned;
static int s_rate_tokens = MCP_RATE_CAPACITY;
static int64_t s_rate_last_refill_us;

void mcp_auth_reset_rate_limit(void)
{
    s_rate_tokens = MCP_RATE_CAPACITY;
    s_rate_last_refill_us = esp_timer_get_time();
}

static char *configured_token(void)
{
    nvs_handle_t handle;
    if (nvs_open(MCP_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = 0;
        if (nvs_get_str(handle, "token", NULL, &len) == ESP_OK && len > 1) {
            char *token = malloc(len);
            if (token != NULL &&
                nvs_get_str(handle, "token", token, &len) == ESP_OK) {
                nvs_close(handle);
                return token;
            }
            free(token);
        }
        nvs_close(handle);
    }
    return CONFIG_MCP_AUTH_TOKEN[0] != '\0' ? strdup(CONFIG_MCP_AUTH_TOKEN)
                                            : NULL;
}

static bool constant_time_equal(const char *a, const char *b)
{
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    size_t max_len = len_a > len_b ? len_a : len_b;
    volatile uint8_t diff = (uint8_t)(len_a ^ len_b);
    for (size_t i = 0; i < max_len; i++) {
        uint8_t ca = i < len_a ? (uint8_t)a[i] : 0;
        uint8_t cb = i < len_b ? (uint8_t)b[i] : 0;
        diff |= (uint8_t)(ca ^ cb);
    }
    return diff == 0;
}

static bool rate_limit_allow(void)
{
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - s_rate_last_refill_us;
    if (elapsed >= MCP_RATE_WINDOW_US) {
        long refill =
            (long)(elapsed / MCP_RATE_WINDOW_US) * CONFIG_MCP_RATE_LIMIT_RPS;
        s_rate_tokens += (int)refill;
        if (s_rate_tokens > MCP_RATE_CAPACITY) s_rate_tokens = MCP_RATE_CAPACITY;
        s_rate_last_refill_us = now;
    }
    if (s_rate_tokens <= 0) return false;
    s_rate_tokens--;
    return true;
}

static bool host_in_allowlist(const char *host)
{
    char normalized[128];
    size_t out = 0;
    if (host[0] == '[') {
        const char *bracket_end = strchr(host, ']');
        if (bracket_end == NULL) return false;
        size_t literal_len = (size_t)(bracket_end - host) + 1;
        if (literal_len >= sizeof(normalized)) return false;
        memcpy(normalized, host, literal_len);
        out = literal_len;
    } else {
        const char *colon = strchr(host, ':');
        size_t host_len = colon != NULL ? (size_t)(colon - host)
                                        : strlen(host);
        if (host_len >= sizeof(normalized)) return false;
        memcpy(normalized, host, host_len);
        out = host_len;
    }
    normalized[out] = '\0';

    const char *entry = CONFIG_MCP_HOST_ALLOWLIST;
    while (*entry != '\0') {
        char candidate[sizeof(normalized)];
        size_t i = 0;
        while (*entry != ',' && *entry != '\0' && i + 1 < sizeof(candidate)) {
            candidate[i++] = (char)tolower((unsigned char)*entry++);
        }
        candidate[i] = '\0';
        if (*entry == ',') entry++;
        bool all_whitespace = true;
        for (size_t j = 0; j < i; j++) {
            if (!isspace((unsigned char)candidate[j])) {
                all_whitespace = false;
                break;
            }
        }
        if (all_whitespace) continue;
        if (strcasecmp(normalized, candidate) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Strict Content-Type parser (§12.7)
// ---------------------------------------------------------------------------

// Only accept: application/json or application/json; charset=utf-8
// Rejects: "application/json-extra", "application/json; charset=utf-8; blah"
static bool validate_content_type(const char *ct)
{
    if (ct == NULL) return false;

    // Skip leading whitespace
    while (*ct == ' ' || *ct == '\t') ct++;

    // Must start with "application/json" (case-insensitive)
    if (strncasecmp(ct, "application/json", 16) != 0) return false;

    ct += 16;

    // End of string = valid
    if (*ct == '\0') return true;

    // Must be semicolon followed by parameters
    if (*ct != ';') return false;
    ct++;

    // Skip whitespace after semicolon
    while (*ct == ' ' || *ct == '\t') ct++;

    // Only accepted parameter: charset=utf-8
    if (strncasecmp(ct, "charset=utf-8", 13) == 0) {
        ct += 13;
        while (*ct == ' ' || *ct == '\t') ct++;
        return (*ct == '\0');
    }

    return false;
}

// ---------------------------------------------------------------------------
// Accept header parser (§12.6)
// ---------------------------------------------------------------------------

typedef struct {
    bool accepts_json;
    bool accepts_event_stream;
} mcp_accept_state_t;

static void parse_accept_header(const char *accept, mcp_accept_state_t *state)
{
    state->accepts_json = false;
    state->accepts_event_stream = false;

    if (accept == NULL) return;

    while (*accept != '\0') {
        // Skip leading comma / whitespace
        while (*accept == ' ' || *accept == '\t' || *accept == ',') accept++;
        if (*accept == '\0') break;

        // Read media type (up to ';' or end)
        const char *start = accept;
        while (*accept != ';' && *accept != ',' && *accept != '\0' &&
               *accept != ' ' && *accept != '\t') {
            accept++;
        }
        size_t len = (size_t)(accept - start);

        // Case-insensitive compare
        if (len == 16 &&
            strncasecmp(start, "application/json", 16) == 0) {
            state->accepts_json = true;
        } else if (len == 24 &&
                   strncasecmp(start, "text/event-stream", 17) == 0) {
            state->accepts_event_stream = true;
        }

        // Skip parameters (q=, charset=, etc.)
        while (*accept != ',' && *accept != '\0') accept++;
    }
}

static mcp_gate_status_t validate_accept(httpd_req_t *req,
                                         const mcp_transport_t *io)
{
    char *accept = io->get_header(req, "Accept");
    mcp_accept_state_t state;
    parse_accept_header(accept, &state);
    free(accept);

    // Default mode: accept if client can receive JSON (or no Accept header)
    if (!CONFIG_MCP_STRICT_ACCEPT_HEADER) {
        // Missing Accept or accepts JSON -> OK
        if (state.accepts_json || (!state.accepts_json && !state.accepts_event_stream)) {
            return MCP_GATE_OK;
        }
        // Accept present but doesn't include JSON -> 406
        return MCP_GATE_BAD_ACCEPT;
    }

    // Strict mode: must advertise both JSON and event-stream
    if (state.accepts_json && state.accepts_event_stream) {
        return MCP_GATE_OK;
    }
    return MCP_GATE_BAD_ACCEPT;
}

// ---------------------------------------------------------------------------
// Host / Origin validation
// ---------------------------------------------------------------------------

static mcp_gate_status_t validate_host_origin(httpd_req_t *req,
                                              const mcp_transport_t *io)
{
    if (CONFIG_MCP_HOST_ALLOWLIST[0] == '\0') return MCP_GATE_OK;

    char *host = io->get_header(req, "Host");
    bool host_ok = host != NULL && host_in_allowlist(host);
    free(host);
    if (!host_ok) return MCP_GATE_FORBIDDEN_HOST;

    char *origin = io->get_header(req, "Origin");
    if (origin != NULL) {
        const char *authority = strstr(origin, "://");
        authority = authority != NULL ? authority + 3 : origin;
        bool origin_ok = host_in_allowlist(authority);
        free(origin);
        if (!origin_ok) return MCP_GATE_FORBIDDEN_HOST;
    }
    return MCP_GATE_OK;
}

// ---------------------------------------------------------------------------
// Main gate (§13.1)
// ---------------------------------------------------------------------------

mcp_gate_status_t mcp_auth_gate(httpd_req_t *req)
{
    const mcp_transport_t *io = mcp_transport_get();

    // Strict Content-Type validation (§12.7)
    char *content_type = io->get_header(req, "Content-Type");
    bool type_ok = validate_content_type(content_type);
    free(content_type);
    if (!type_ok) return MCP_GATE_BAD_CONTENT_TYPE;

    if (!rate_limit_allow()) return MCP_GATE_RATE_LIMITED;

    char *token = configured_token();
    if (token == NULL) {
        if (!s_dev_mode_warned) {
            ESP_LOGW(TAG,
                     "no MCP auth token configured: /mcp runs in dev mode "
                     "without authentication");
            s_dev_mode_warned = true;
        }
    } else {
        char *authorization = io->get_header(req, "Authorization");
        const char *scheme = "Bearer ";
        bool authorized =
            authorization != NULL &&
            strncasecmp(authorization, scheme, strlen(scheme)) == 0 &&
            constant_time_equal(authorization + strlen(scheme), token);
        free(authorization);
        free(token);
        if (!authorized) return MCP_GATE_UNAUTHORIZED;
    }

    mcp_gate_status_t host_result = validate_host_origin(req, io);
    if (host_result != MCP_GATE_OK) return host_result;

    return validate_accept(req, io);
}
