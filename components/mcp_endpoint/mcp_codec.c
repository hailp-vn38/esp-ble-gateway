#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"

#include "mcp_endpoint_internal.h"

// ---------------------------------------------------------------------------
// Base64 decoding (minimal, for Mcp-Name sentinel values)
// ---------------------------------------------------------------------------

static int b64_char_val(char c)
{
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
    if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *in, size_t len, uint8_t *out, size_t out_max)
{
    size_t out_len = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (size_t i = 0; i < len; i++) {
        if (in[i] == '=') break;
        int val = b64_char_val(in[i]);
        if (val < 0) return -1;
        acc = (acc << 6) | (uint32_t)val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            uint8_t byte = (uint8_t)((acc >> bits) & 0xFF);
            if (out_len >= out_max) return -1;
            out[out_len++] = byte;
        }
    }
    return (int)out_len;
}

// MCP 2026-07-28 Base64 sentinel: =?base64?<encoded>?=
static const char MCP_B64_PREFIX[] = "=?base64?";
static const char MCP_B64_SUFFIX[] = "?=";
#define MCP_B64_PREFIX_LEN 10
#define MCP_B64_SUFFIX_LEN 2

char *mcp_codec_decode_name(const char *raw_name)
{
    if (raw_name == NULL) return NULL;

    // Check for =?base64?...?= sentinel
    if (strncmp(raw_name, MCP_B64_PREFIX, MCP_B64_PREFIX_LEN) == 0) {
        size_t raw_len = strlen(raw_name);
        if (raw_len < MCP_B64_PREFIX_LEN + MCP_B64_SUFFIX_LEN) return NULL;
        if (strcmp(raw_name + raw_len - MCP_B64_SUFFIX_LEN,
                   MCP_B64_SUFFIX) != 0) {
            return NULL;
        }
        const char *encoded = raw_name + MCP_B64_PREFIX_LEN;
        size_t enc_len = raw_len - MCP_B64_PREFIX_LEN - MCP_B64_SUFFIX_LEN;
        // Strip trailing '=' padding is handled by b64_decode.
        size_t max_decoded = (enc_len * 3 / 4) + 1;
        if (max_decoded > MCP_NAME_DECODED_MAX + 1) return NULL;
        char *decoded = malloc(max_decoded + 1);
        if (decoded == NULL) return NULL;
        int result = b64_decode(encoded, enc_len, (uint8_t *)decoded,
                                max_decoded);
        if (result < 0 || result > MCP_NAME_DECODED_MAX) {
            free(decoded);
            return NULL;
        }
        decoded[result] = '\0';
        return decoded;
    }

    // Plain header-safe value
    return strdup(raw_name);
}

// ---------------------------------------------------------------------------
// Header helpers
// ---------------------------------------------------------------------------

static char *request_header(httpd_req_t *req, const char *name)
{
    return mcp_transport_get()->get_header(req, name);
}

// ---------------------------------------------------------------------------
// Protocol-era detection (§7.2 — body-aware, no silent fallback)
// ---------------------------------------------------------------------------

// Detect modern markers in the parsed body.
static bool body_has_2026_markers(const cJSON *root)
{
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (params == NULL || !cJSON_IsObject(params)) return false;

    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
    if (meta == NULL || !cJSON_IsObject(meta)) return false;

    // _meta.io.modelcontextprotocol/protocolVersion present
    const cJSON *proto_ver =
        cJSON_GetObjectItemCaseSensitive(meta, MCP_META_KEY_PROTOCOL_VERSION);
    if (proto_ver != NULL && cJSON_IsString(proto_ver)) return true;

    // _meta.io.modelcontextprotocol/clientCapabilities present
    const cJSON *client_caps =
        cJSON_GetObjectItemCaseSensitive(meta, MCP_META_KEY_CLIENT_CAPS);
    if (client_caps != NULL && cJSON_IsObject(client_caps)) return true;

    return false;
}

static bool has_mcp_method_header(const mcp_request_context_t *ctx)
{
    return ctx->has_method_header;
}

static bool has_mcp_name_header(const mcp_request_context_t *ctx)
{
    return ctx->has_name_header;
}

int mcp_protocol_detect(httpd_req_t *req, const cJSON *root,
                        mcp_request_context_t *ctx,
                        mcp_rpc_error_detail_t *error)
{
    memset(ctx, 0, sizeof(*ctx));

    const cJSON *method_item = cJSON_GetObjectItemCaseSensitive(root, "method");
    const char *method_str =
        (method_item != NULL && cJSON_IsString(method_item))
            ? method_item->valuestring
            : NULL;

    // Read optional headers
    char *version_header = request_header(req, "MCP-Protocol-Version");
    char *method_header = request_header(req, "Mcp-Method");
    char *name_header   = request_header(req, "Mcp-Name");

    if (version_header != NULL) {
        strlcpy(ctx->protocol_version, version_header,
                sizeof(ctx->protocol_version));
        ctx->has_protocol_header = true;
    }
    if (method_header != NULL) {
        strlcpy(ctx->mcp_method, method_header, sizeof(ctx->mcp_method));
        ctx->has_method_header = true;
    }
    if (name_header != NULL) {
        // Decode potentially Base64-encoded name
        char *decoded = mcp_codec_decode_name(name_header);
        if (decoded != NULL) {
            strlcpy(ctx->mcp_name, decoded, sizeof(ctx->mcp_name));
            free(decoded);
        } else {
            // Decode failure — store raw for error reporting
            strlcpy(ctx->mcp_name, name_header, sizeof(ctx->mcp_name));
        }
        ctx->has_name_header = true;
        free(name_header);
    }
    free(method_header);
    free(version_header);

    // Notification detection: no "id" field
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    ctx->notification = (id == NULL);

    // --- Detection rules (§7.2) ---

    // Rule 1: method == "initialize" -> 2025 compat entry point
    if (method_str != NULL && strcmp(method_str, "initialize") == 0) {
    if (!CONFIG_MCP_COMPAT_2025) {
        error->rpc_code = -32601;
        error->message = "Method not found";
        error->http_status = "404 Not Found";
        return -32601;
    }
    ctx->era = MCP_ERA_2025_11_25;
    ctx->initialize_request = true;
    return 0;
    }

    // Rule 2: explicit MCP-Protocol-Version header present
    if (ctx->has_protocol_header) {
        if (strcmp(ctx->protocol_version, MCP_PROTOCOL_VERSION_2026) == 0) {
            ctx->era = MCP_ERA_2026_07_28;
        } else if (strcmp(ctx->protocol_version, MCP_PROTOCOL_VERSION_2025) ==
                   0) {
            if (!CONFIG_MCP_COMPAT_2025) {
                cJSON *data =
                    mcp_protocol_build_unsupported_version_data("2025-11-25");
                error->rpc_code = MCP_ERR_UNSUPPORTED_VERSION;
                error->message = "Unsupported protocol version";
                error->http_status = "400 Bad Request";
                error->data = data;
                return MCP_ERR_UNSUPPORTED_VERSION;
            }
            ctx->era = MCP_ERA_2025_11_25;
        } else {
            // Unsupported explicit version
            cJSON *data = mcp_protocol_build_unsupported_version_data(
                ctx->protocol_version);
            error->rpc_code = MCP_ERR_UNSUPPORTED_VERSION;
            error->message = "Unsupported protocol version";
            error->http_status = "400 Bad Request";
            error->data = data;
            return MCP_ERR_UNSUPPORTED_VERSION;
        }
        return 0;
    }

    // Rule 3: modern body markers but missing protocol header -> -32020
    if (body_has_2026_markers(root) || has_mcp_method_header(ctx) ||
        has_mcp_name_header(ctx)) {
        error->rpc_code = MCP_ERR_HEADER;
        error->message = "Missing MCP-Protocol-Version header";
        error->http_status = "400 Bad Request";
        return MCP_ERR_HEADER;
    }

    // Rule 4: no reliable era signal
    if (!CONFIG_MCP_COMPAT_2025) {
        error->rpc_code = MCP_ERR_UNSUPPORTED_VERSION;
        error->message = "Unsupported protocol version";
        error->http_status = "400 Bad Request";
        return MCP_ERR_UNSUPPORTED_VERSION;
    }

    // Compat mode: treat versionless non-initialize requests as 2025
    ctx->era = MCP_ERA_2025_11_25;
    return 0;
}

// ---------------------------------------------------------------------------
// Era-specific validation
// ---------------------------------------------------------------------------

int mcp_protocol_validate_request(const cJSON *root,
                                  const mcp_request_context_t *ctx,
                                  mcp_rpc_error_detail_t *error)
{
    if (ctx->era == MCP_ERA_2026_07_28) {
        // Validate required _meta fields (§11)
        const cJSON *params =
            cJSON_GetObjectItemCaseSensitive(root, "params");
        if (params == NULL || !cJSON_IsObject(params)) {
            error->rpc_code = -32602;
            error->message = "Invalid params";
            error->http_status = "400 Bad Request";
            return -32602;
        }

        const cJSON *meta =
            cJSON_GetObjectItemCaseSensitive(params, "_meta");
        if (meta == NULL || !cJSON_IsObject(meta)) {
            error->rpc_code = -32602;
            error->message = "Missing required _meta";
            error->http_status = "400 Bad Request";
            return -32602;
        }

        // protocolVersion required, must be "2026-07-28"
        const cJSON *proto_ver = cJSON_GetObjectItemCaseSensitive(
            meta, MCP_META_KEY_PROTOCOL_VERSION);
        if (proto_ver == NULL || !cJSON_IsString(proto_ver)) {
            error->rpc_code = -32602;
            error->message = "Missing _meta.protocolVersion";
            error->http_status = "400 Bad Request";
            return -32602;
        }
        if (strcmp(proto_ver->valuestring, MCP_PROTOCOL_VERSION_2026) != 0) {
            cJSON *data = mcp_protocol_build_unsupported_version_data(
                proto_ver->valuestring);
            error->rpc_code = MCP_ERR_UNSUPPORTED_VERSION;
            error->message = "Unsupported protocol version";
            error->http_status = "400 Bad Request";
            error->data = data;
            return MCP_ERR_UNSUPPORTED_VERSION;
        }

        // clientCapabilities required
        const cJSON *client_caps = cJSON_GetObjectItemCaseSensitive(
            meta, MCP_META_KEY_CLIENT_CAPS);
        if (client_caps == NULL || !cJSON_IsObject(client_caps)) {
            error->rpc_code = -32602;
            error->message = "Missing _meta.clientCapabilities";
            error->http_status = "400 Bad Request";
            return -32602;
        }

        // Mcp-Method required and must match body "method"
        if (!ctx->has_method_header || ctx->mcp_method[0] == '\0') {
            error->rpc_code = MCP_ERR_HEADER;
            error->message = "Missing Mcp-Method header";
            error->http_status = "400 Bad Request";
            return MCP_ERR_HEADER;
        }
        const cJSON *method_item =
            cJSON_GetObjectItemCaseSensitive(root, "method");
        if (method_item == NULL || !cJSON_IsString(method_item) ||
            strcmp(ctx->mcp_method, method_item->valuestring) != 0) {
            error->rpc_code = MCP_ERR_HEADER;
            error->message = "Mcp-Method mismatch";
            error->http_status = "400 Bad Request";
            return MCP_ERR_HEADER;
        }

        // Mcp-Name required for tools/call, must match params.name
        if (strcmp(method_item->valuestring, "tools/call") == 0) {
            if (!ctx->has_name_header) {
                error->rpc_code = MCP_ERR_HEADER;
                error->message = "Missing Mcp-Name header";
                error->http_status = "400 Bad Request";
                return MCP_ERR_HEADER;
            }
            const cJSON *params_obj =
                cJSON_GetObjectItemCaseSensitive(root, "params");
            const cJSON *name_item =
                cJSON_GetObjectItemCaseSensitive(params_obj, "name");
            if (name_item == NULL || !cJSON_IsString(name_item)) {
                error->rpc_code = -32602;
                error->message = "Missing params.name";
                error->http_status = "400 Bad Request";
                return -32602;
            }
            if (strcmp(ctx->mcp_name, name_item->valuestring) != 0) {
                error->rpc_code = MCP_ERR_HEADER;
                error->message = "Mcp-Name mismatch";
                error->http_status = "400 Bad Request";
                return MCP_ERR_HEADER;
            }
        }
    }

    return 0;  // valid
}

// ---------------------------------------------------------------------------
// MCP 2025 compatibility: initialize handler
// ---------------------------------------------------------------------------

cJSON *mcp_protocol_build_initialize_result(const cJSON *params,
                                            mcp_rpc_error_detail_t *error)
{
    if (!CONFIG_MCP_COMPAT_2025) {
        error->rpc_code = -32601;
        error->message = "Method not found";
        return NULL;
    }

    // Validate required initialize fields
    if (params == NULL || !cJSON_IsObject(params)) {
        error->rpc_code = -32602;
        error->message = "Invalid params";
        return NULL;
    }

    const cJSON *client_info =
        cJSON_GetObjectItemCaseSensitive(params, "clientInfo");
    if (client_info == NULL || !cJSON_IsObject(client_info)) {
        error->rpc_code = -32602;
        error->message = "Missing clientInfo";
        return NULL;
    }
    const cJSON *name =
        cJSON_GetObjectItemCaseSensitive(client_info, "name");
    if (name == NULL || !cJSON_IsString(name)) {
        error->rpc_code = -32602;
        error->message = "Missing clientInfo.name";
        return NULL;
    }
    const cJSON *version =
        cJSON_GetObjectItemCaseSensitive(client_info, "version");
    if (version == NULL || !cJSON_IsString(version)) {
        error->rpc_code = -32602;
        error->message = "Missing clientInfo.version";
        return NULL;
    }

    // Build InitializeResult — always counter-offer 2025-11-25 (§8.1)
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        error->rpc_code = -32603;
        error->message = "Out of memory";
        return NULL;
    }

    cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION_2025);

    cJSON *capabilities = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateObject();
    if (capabilities == NULL || tools == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(capabilities);
        cJSON_Delete(tools);
        error->rpc_code = -32603;
        error->message = "Out of memory";
        return NULL;
    }
    cJSON_AddBoolToObject(tools, "listChanged", false);
    cJSON_AddItemToObject(capabilities, "tools", tools);
    cJSON_AddItemToObject(result, "capabilities", capabilities);

    cJSON *server_info = cJSON_CreateObject();
    if (server_info == NULL) {
        cJSON_Delete(result);
        error->rpc_code = -32603;
        error->message = "Out of memory";
        return NULL;
    }
    cJSON_AddStringToObject(server_info, "name", MCP_SERVER_NAME);
    cJSON_AddStringToObject(server_info, "version", MCP_SERVER_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", server_info);

    cJSON_AddStringToObject(
        result, "instructions",
        "Controls BLE devices managed by this ESP32 gateway.");

    return result;
}

// ---------------------------------------------------------------------------
// -32022 error.data builder
// ---------------------------------------------------------------------------

cJSON *mcp_protocol_build_unsupported_version_data(const char *requested)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) return NULL;

    cJSON *supported = cJSON_CreateArray();
    if (supported == NULL) {
        cJSON_Delete(data);
        return NULL;
    }
    cJSON_AddItemToArray(supported, cJSON_CreateString(MCP_PROTOCOL_VERSION_2026));
    if (CONFIG_MCP_COMPAT_2025) {
        cJSON_AddItemToArray(supported,
                             cJSON_CreateString(MCP_PROTOCOL_VERSION_2025));
    }
    cJSON_AddItemToObject(data, "supported", supported);

    cJSON_AddStringToObject(data, "requested",
                            requested != NULL ? requested : "unknown");
    return data;
}

// ---------------------------------------------------------------------------
// serverInfo helper — adds to result._meta (§12.4)
// ---------------------------------------------------------------------------

bool mcp_result_add_server_info(cJSON *result)
{
    cJSON *meta = cJSON_CreateObject();
    cJSON *info = cJSON_CreateObject();
    if (meta == NULL || info == NULL) {
        cJSON_Delete(meta);
        cJSON_Delete(info);
        return false;
    }
    cJSON_AddStringToObject(info, "name", MCP_SERVER_NAME);
    cJSON_AddStringToObject(info, "version", MCP_SERVER_VERSION);
    cJSON_AddItemToObject(meta, MCP_META_KEY_SERVER_INFO, info);
    cJSON_AddItemToObject(result, "_meta", meta);
    return true;
}

// ---------------------------------------------------------------------------
// server/discover — reflects actual CONFIG_MCP_COMPAT_2025 (§11.1)
// ---------------------------------------------------------------------------

cJSON *mcp_codec_build_discovery(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *capabilities = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateObject();
    if (result == NULL || capabilities == NULL || tools == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(capabilities);
        cJSON_Delete(tools);
        return NULL;
    }

    cJSON_AddStringToObject(result, "resultType", "complete");

    cJSON *supported_versions = cJSON_CreateArray();
    cJSON_AddItemToArray(supported_versions,
                         cJSON_CreateString(MCP_PROTOCOL_VERSION_2026));
    if (CONFIG_MCP_COMPAT_2025) {
        cJSON_AddItemToArray(supported_versions,
                             cJSON_CreateString(MCP_PROTOCOL_VERSION_2025));
    }
    cJSON_AddItemToObject(result, "supportedVersions", supported_versions);

    cJSON_AddBoolToObject(tools, "listChanged", false);
    cJSON_AddItemToObject(capabilities, "tools", tools);
    cJSON_AddItemToObject(result, "capabilities", capabilities);

    if (!mcp_result_add_server_info(result)) {
        cJSON_Delete(result);
        return NULL;
    }

    cJSON_AddStringToObject(
        result, "instructions",
        "Controls BLE devices managed by this ESP32 gateway.");
    cJSON_AddNumberToObject(result, "ttlMs", MCP_TOOLS_CACHE_TTL_MS);
    cJSON_AddStringToObject(result, "cacheScope", MCP_TOOLS_CACHE_SCOPE);

    return result;
}
