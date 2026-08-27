#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "nvs.h"

#include "sdkconfig.h"

#include "esp_log.h"
#include "mcp_endpoint_internal.h"

static const char *TAG = "mcp_codec";

#define MCP_NVS_NAMESPACE "mcp"
#define MCP_NVS_LEGACY_KEY "legacy"

// Legacy mode resolution order: NVS runtime override -> Kconfig default.
// The override is read per request so an OTA-era flip takes effect without a
// reboot; failures fall back to the compiled default.
bool mcp_codec_legacy_enabled(void)
{
    static bool warned = false;
    nvs_handle_t handle;
    bool legacy = false;
    if (nvs_open(MCP_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t value = 0;
        esp_err_t err = nvs_get_u8(handle, MCP_NVS_LEGACY_KEY, &value);
        nvs_close(handle);
        if (err == ESP_OK) legacy = (value != 0);
    }
    if (!legacy) legacy = CONFIG_MCP_LEGACY_MODE;
    if (legacy && !warned) {
        ESP_LOGW(TAG,
                 "DEPRECATED: MCP legacy mode is active and will be removed "
                 "in a future release. Set MCP_LEGACY_MODE=n or use "
                 "MCP-Protocol-Version: 2026-07-28 header.");
        warned = true;
    }
    return legacy;
}

int mcp_codec_set_legacy_override(int value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MCP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return -1;
    if (value < 0) {
        err = nvs_erase_key(handle, MCP_NVS_LEGACY_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_u8(handle, MCP_NVS_LEGACY_KEY, value != 0 ? 1 : 0);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK ? 0 : -1;
}

static char *request_header(httpd_req_t *req, const char *name)
{
    return mcp_transport_get()->get_header(req, name);
}

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

// Decode a Base64 string. Returns number of output bytes written to `out`,
// or -1 on invalid input. `out` must have at least (len * 3 / 4 + 1) bytes.
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

// MCP sentinel prefix for Base64-encoded names: \x00 followed by "b64:"
// When Mcp-Name header starts with this, the rest is Base64-encoded.
static const char MCP_B64_SENTINEL[] = {0x00, 'b', '6', '4', ':', '\0'};
#define MCP_B64_SENTINEL_LEN 5

char *mcp_protocol_decode_name(const char *raw_name)
{
    if (raw_name == NULL) return NULL;

    // Check for Base64 sentinel prefix
    if (strncmp(raw_name, MCP_B64_SENTINEL, MCP_B64_SENTINEL_LEN) == 0) {
        const char *encoded = raw_name + MCP_B64_SENTINEL_LEN;
        size_t enc_len = strlen(encoded);
        // Max decoded size: enc_len * 3/4 rounded up
        size_t max_decoded = (enc_len * 3 / 4) + 1;
        if (max_decoded > 256) return NULL;  // sanity limit
        char *decoded = malloc(max_decoded + 1);
        if (decoded == NULL) return NULL;
        int result = b64_decode(encoded, enc_len, (uint8_t *)decoded,
                                max_decoded);
        if (result < 0) {
            free(decoded);
            return NULL;
        }
        decoded[result] = '\0';
        return decoded;
    }

    // Plain header-safe value — return a copy
    return strdup(raw_name);
}

// ---------------------------------------------------------------------------
// Header parsing
// ---------------------------------------------------------------------------

int mcp_codec_parse_meta(httpd_req_t *req, mcp_request_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));

    char *version = request_header(req, "MCP-Protocol-Version");
    if (version == NULL) {
        // No header: legacy clients keep working only in legacy mode.
        return mcp_codec_legacy_enabled() ? 0 : MCP_ERR_UNSUPPORTED_VERSION;
    }
    bool supported = strcmp(version, MCP_PROTOCOL_VERSION_2026) == 0;
    free(version);
    if (!supported) return MCP_ERR_UNSUPPORTED_VERSION;

    meta->mcp_2026 = true;

    char *method = request_header(req, "Mcp-Method");
    if (method != NULL) {
        strlcpy(meta->mcp_method, method, sizeof(meta->mcp_method));
        free(method);
    }
    char *name = request_header(req, "Mcp-Name");
    if (name != NULL) {
        strlcpy(meta->mcp_name, name, sizeof(meta->mcp_name));
        meta->has_name = true;
        free(name);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Protocol validation — required _meta fields
// ---------------------------------------------------------------------------

int mcp_protocol_validate_meta(const cJSON *root)
{
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (params == NULL || !cJSON_IsObject(params)) {
        return -32602;  // Invalid params
    }

    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
    if (meta == NULL || !cJSON_IsObject(meta)) {
        return -32602;  // Invalid params — missing required _meta
    }

    // protocolVersion: required string, must equal "2026-07-28"
    // MCP 2026-07-28 uses "io.modelcontextprotocol/protocolVersion" as key
    const cJSON *proto_ver =
        cJSON_GetObjectItemCaseSensitive(meta, MCP_META_KEY_PROTOCOL_VERSION);
    if (proto_ver == NULL || !cJSON_IsString(proto_ver)) {
        return -32602;  // Invalid params — missing protocolVersion
    }
    if (strcmp(proto_ver->valuestring, MCP_PROTOCOL_VERSION_2026) != 0) {
        return MCP_ERR_UNSUPPORTED_VERSION;
    }

    // clientCapabilities: required object
    // MCP 2026-07-28 uses "io.modelcontextprotocol/clientCapabilities" as key
    const cJSON *client_caps =
        cJSON_GetObjectItemCaseSensitive(meta, MCP_META_KEY_CLIENT_CAPS);
    if (client_caps == NULL || !cJSON_IsObject(client_caps)) {
        return -32602;  // Invalid params — missing clientCapabilities
    }

    return 0;  // valid
}

// ---------------------------------------------------------------------------
// Header/body consistency validation
// ---------------------------------------------------------------------------

int mcp_protocol_validate_headers(const cJSON *root,
                                  const mcp_request_meta_t *meta)
{
    if (!meta->mcp_2026) return 0;  // legacy mode — skip header checks

    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (method == NULL || !cJSON_IsString(method)) {
        return -32600;  // Invalid Request
    }

    // Mcp-Method required and must match body "method"
    if (meta->mcp_method[0] == '\0') {
        return MCP_ERR_HEADER;  // Missing Mcp-Method
    }
    if (strcmp(meta->mcp_method, method->valuestring) != 0) {
        return MCP_ERR_HEADER;  // Mcp-Method mismatch
    }

    // For tools/call: Mcp-Name required and must match params.name
    if (strcmp(method->valuestring, "tools/call") == 0) {
        if (!meta->has_name) {
            return MCP_ERR_HEADER;  // Missing Mcp-Name for tools/call
        }

        const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        const cJSON *name_item =
            cJSON_GetObjectItemCaseSensitive(params, "name");
        if (name_item == NULL || !cJSON_IsString(name_item)) {
            return -32602;  // Invalid params — missing name
        }

        // Decode the potentially Base64-encoded Mcp-Name
        char *decoded_name = mcp_protocol_decode_name(meta->mcp_name);
        if (decoded_name == NULL) {
            return MCP_ERR_HEADER;  // Base64 decode failure
        }

        bool match = (strcmp(decoded_name, name_item->valuestring) == 0);
        free(decoded_name);

        if (!match) {
            return MCP_ERR_HEADER;  // Mcp-Name mismatch
        }
    }

    return 0;  // valid
}

// ---------------------------------------------------------------------------
// serverInfo helper
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
// server/discover — target MCP 2026-07-28 shape
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
    cJSON_AddItemToObject(result, "supportedVersions", supported_versions);

    cJSON_AddBoolToObject(tools, "listChanged", false);
    cJSON_AddItemToObject(capabilities, "tools", tools);
    cJSON_AddItemToObject(result, "capabilities", capabilities);

    // serverInfo in result._meta per MCP 2026-07-28
    if (!mcp_result_add_server_info(result)) {
        cJSON_Delete(result);
        return NULL;
    }

    cJSON_AddStringToObject(
        result, "instructions",
        "Controls BLE devices managed by this ESP32 gateway.");
    cJSON_AddNumberToObject(result, "ttlMs", 60000);
    cJSON_AddStringToObject(result, "cacheScope", "private");

    return result;
}
