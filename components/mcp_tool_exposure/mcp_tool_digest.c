#include <string.h>

#include "psa/crypto.h"

#include "mcp_tool_exposure_internal.h"

void mcp_tool_digest_compute(const device_schema_tool_t *cap,
                             uint8_t out[MCP_CAPABILITY_DIGEST_LEN])
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t status;

    status = psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (status != PSA_SUCCESS) {
        memset(out, 0, MCP_CAPABILITY_DIGEST_LEN);
        return;
    }

    /* command UTF-8 + NUL */
    size_t cmd_len = strlen(cap->command) + 1;
    psa_hash_update(&op, (const uint8_t *)cap->command, cmd_len);

    /* value_type u8 */
    uint8_t vt = (uint8_t)cap->value_type;
    psa_hash_update(&op, &vt, 1);

    /* flags u8 */
    psa_hash_update(&op, &cap->flags, 1);

    /* min_value LE32 */
    uint8_t buf4[4];
    uint32_t u32;
    u32 = (uint32_t)cap->min_value;
    buf4[0] = (uint8_t)(u32);
    buf4[1] = (uint8_t)(u32 >> 8);
    buf4[2] = (uint8_t)(u32 >> 16);
    buf4[3] = (uint8_t)(u32 >> 24);
    psa_hash_update(&op, buf4, 4);

    /* max_value LE32 */
    u32 = (uint32_t)cap->max_value;
    buf4[0] = (uint8_t)(u32);
    buf4[1] = (uint8_t)(u32 >> 8);
    buf4[2] = (uint8_t)(u32 >> 16);
    buf4[3] = (uint8_t)(u32 >> 24);
    psa_hash_update(&op, buf4, 4);

    /* step LE32 */
    u32 = cap->step;
    buf4[0] = (uint8_t)(u32);
    buf4[1] = (uint8_t)(u32 >> 8);
    buf4[2] = (uint8_t)(u32 >> 16);
    buf4[3] = (uint8_t)(u32 >> 24);
    psa_hash_update(&op, buf4, 4);

    /* unit UTF-8 + NUL */
    size_t unit_len = strlen(cap->unit) + 1;
    psa_hash_update(&op, (const uint8_t *)cap->unit, unit_len);

    /* Finish and truncate to 128-bit (16 bytes). */
    uint8_t full_hash[32];
    size_t hash_len = 0;
    status = psa_hash_finish(&op, full_hash, sizeof(full_hash), &hash_len);
    if (status != PSA_SUCCESS || hash_len < MCP_CAPABILITY_DIGEST_LEN) {
        memset(out, 0, MCP_CAPABILITY_DIGEST_LEN);
        return;
    }

    memcpy(out, full_hash, MCP_CAPABILITY_DIGEST_LEN);
}

bool mcp_tool_digest_match(const uint8_t a[MCP_CAPABILITY_DIGEST_LEN],
                           const uint8_t b[MCP_CAPABILITY_DIGEST_LEN])
{
    uint8_t diff = 0;
    for (size_t i = 0; i < MCP_CAPABILITY_DIGEST_LEN; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}
