#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"

#include "mcp_tool_exposure_internal.h"

static const char *TAG = "mcp_tool_name";

static bool is_allowed_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.';
}

static size_t sanitize_device_id(const char *device_id, char *slug,
                                 size_t slug_len)
{
    size_t out = 0;
    for (const char *p = device_id; *p != '\0' && out < slug_len - 1; p++) {
        char c = *p;
        if (is_allowed_char(c)) {
            slug[out++] = c;
        } else if (c == ' ' || c == '/' || c == '\\' || c == ':' ||
                   c == '@' || c == '#') {
            slug[out++] = '_';
        }
    }
    while (out > 0 && slug[out - 1] == '_') {
        out--;
    }
    slug[out] = '\0';
    return out;
}

static void compute_id_hash(const char *input, char *hex16)
{
    uint8_t hash[32];
    size_t hash_len = 0;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&op, PSA_ALG_SHA_256);
    psa_hash_update(&op, (const uint8_t *)input, strlen(input));
    psa_hash_finish(&op, hash, sizeof(hash), &hash_len);
    for (int i = 0; i < 16; i++) {
        snprintf(hex16 + i * 2, 3, "%02x", hash[i]);
    }
    hex16[32] = '\0';
}

esp_err_t mcp_tool_name_generate(const char *device_id, const char *command,
                                 char *out_name, size_t out_len)
{
    if (device_id == NULL || command == NULL || out_name == NULL ||
        out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t device_len = strlen(device_id);
    size_t command_len = strlen(command);
    size_t dot_len = 1;
    size_t total_len = device_len + dot_len + command_len;

    /* Fast path: valid chars, non-empty, fits within limit. */
    bool valid = device_len > 0 && command_len > 0;
    if (valid) {
        for (size_t i = 0; i < device_len && valid; i++) {
            if (!is_allowed_char(device_id[i])) valid = false;
        }
    }
    if (valid && total_len <= MCP_DYNAMIC_TOOL_NAME_MAX) {
        snprintf(out_name, out_len, "%s.%s", device_id, command);
        return ESP_OK;
    }

    /* Sanitized path: slug_hash16.command */
    char slug[GW_MSG_DEVICE_ID_LEN];
    size_t slug_len = sanitize_device_id(device_id, slug, sizeof(slug));

    char hash_hex[33];
    compute_id_hash(device_id, hash_hex);

    /* suffix = _hash16.command */
    size_t suffix_len = 1 + 32 + 1 + command_len;
    size_t max_slug = MCP_DYNAMIC_TOOL_NAME_MAX - suffix_len;
    if (slug_len > max_slug) slug_len = max_slug;

    if (slug_len == 0) {
        ESP_LOGE(TAG, "Empty slug for device_id '%s'", device_id);
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(out_name, out_len, "%.*s_%s.%s",
             (int)slug_len, slug, hash_hex, command);

    if (strlen(out_name) > MCP_DYNAMIC_TOOL_NAME_MAX) {
        ESP_LOGE(TAG, "Generated tool name exceeds limit for '%s'", device_id);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}
