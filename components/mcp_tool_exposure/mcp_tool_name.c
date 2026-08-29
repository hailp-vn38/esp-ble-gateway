#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device_store.h"
#include "mcp_tool_exposure_internal.h"

static bool is_allowed_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.';
}

static uint32_t utf8_next(const unsigned char **cursor)
{
    const unsigned char *p = *cursor;
    uint32_t codepoint;
    if (p[0] < 0x80) {
        *cursor = p + 1;
        return p[0];
    }
    if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
        *cursor = p + 2;
        return codepoint;
    }
    if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 &&
        (p[2] & 0xc0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x0f) << 12) |
                    ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        *cursor = p + 3;
        return codepoint;
    }
    *cursor = p + 1;
    return 0;
}

/* Keep MCP tool names portable while preserving common Vietnamese names. */
static char fold_vietnamese(uint32_t cp)
{
    switch (cp) {
    case 0x00c0: case 0x00c1: case 0x00c2: case 0x00c3: case 0x0102:
    case 0x1ea0: case 0x1ea2: case 0x1ea4: case 0x1ea6: case 0x1ea8:
    case 0x1eaa: case 0x1eac: case 0x1eae: case 0x1eb0: case 0x1eb2:
    case 0x1eb4: case 0x1eb6:
        return 'A';
    case 0x00e0: case 0x00e1: case 0x00e2: case 0x00e3: case 0x0103:
    case 0x1ea1: case 0x1ea3: case 0x1ea5: case 0x1ea7: case 0x1ea9:
    case 0x1eab: case 0x1ead: case 0x1eaf: case 0x1eb1: case 0x1eb3:
    case 0x1eb5: case 0x1eb7:
        return 'a';
    case 0x0110:
        return 'D';
    case 0x0111:
        return 'd';
    case 0x00c8: case 0x00c9: case 0x00ca: case 0x1eb8: case 0x1eba:
    case 0x1ebc: case 0x1ebe: case 0x1ec0: case 0x1ec2: case 0x1ec4:
    case 0x1ec6:
        return 'E';
    case 0x00e8: case 0x00e9: case 0x00ea: case 0x1eb9: case 0x1ebb:
    case 0x1ebd: case 0x1ebf: case 0x1ec1: case 0x1ec3: case 0x1ec5:
    case 0x1ec7:
        return 'e';
    case 0x00cc: case 0x00cd: case 0x0128: case 0x1ec8: case 0x1eca:
        return 'I';
    case 0x00ec: case 0x00ed: case 0x0129: case 0x1ec9: case 0x1ecb:
        return 'i';
    case 0x00d2: case 0x00d3: case 0x00d4: case 0x00d5: case 0x01a0:
    case 0x1ecc: case 0x1ece: case 0x1ed0: case 0x1ed2: case 0x1ed4:
    case 0x1ed6: case 0x1ed8: case 0x1eda: case 0x1edc: case 0x1ede:
    case 0x1ee0: case 0x1ee2:
        return 'O';
    case 0x00f2: case 0x00f3: case 0x00f4: case 0x00f5: case 0x01a1:
    case 0x1ecd: case 0x1ecf: case 0x1ed1: case 0x1ed3: case 0x1ed5:
    case 0x1ed7: case 0x1ed9: case 0x1edb: case 0x1edd: case 0x1edf:
    case 0x1ee1: case 0x1ee3:
        return 'o';
    case 0x00d9: case 0x00da: case 0x0168: case 0x01af: case 0x1ee4:
    case 0x1ee6: case 0x1ee8: case 0x1eea: case 0x1eec: case 0x1eee:
    case 0x1ef0:
        return 'U';
    case 0x00f9: case 0x00fa: case 0x0169: case 0x01b0: case 0x1ee5:
    case 0x1ee7: case 0x1ee9: case 0x1eeb: case 0x1eed: case 0x1eef:
    case 0x1ef1:
        return 'u';
    case 0x00dd: case 0x1ef2: case 0x1ef4: case 0x1ef6: case 0x1ef8:
        return 'Y';
    case 0x00fd: case 0x1ef3: case 0x1ef5: case 0x1ef7: case 0x1ef9:
        return 'y';
    default:
        return 0;
    }
}

static size_t sanitize_device_name(const char *device_name, char *slug,
                                   size_t slug_capacity)
{
    size_t out = 0;
    bool separator_pending = false;
    const unsigned char *cursor = (const unsigned char *)device_name;
    while (*cursor != '\0') {
        uint32_t cp = utf8_next(&cursor);
        char folded = cp < 0x80 ? (char)cp : fold_vietnamese(cp);
        if (folded != 0 && is_allowed_char(folded)) {
            if (separator_pending && out > 0 && out < slug_capacity - 1) {
                slug[out++] = '_';
            }
            separator_pending = false;
            if (out < slug_capacity - 1) slug[out++] = folded;
        } else {
            separator_pending = true;
        }
    }
    while (out > 0 && (slug[out - 1] == '_' || slug[out - 1] == '-' ||
                       slug[out - 1] == '.')) {
        out--;
    }
    slug[out] = '\0';
    return out;
}

esp_err_t mcp_tool_name_generate(const char *device_name, const char *command,
                                 char *out_name, size_t out_len)
{
    if (device_name == NULL || command == NULL || out_name == NULL ||
        out_len == 0 || command[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    for (const char *p = command; *p != '\0'; p++) {
        if (!is_allowed_char(*p)) return ESP_ERR_INVALID_ARG;
    }

    char device_slug[DEVICE_NAME_MAX_LEN];
    if (sanitize_device_name(device_name, device_slug, sizeof(device_slug)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out_name, out_len, "%s_%s", command, device_slug);
    if (written < 0 || (size_t)written >= out_len ||
        written > MCP_DYNAMIC_TOOL_NAME_MAX) {
        if (out_len > 0) out_name[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
