#ifndef MCP_TOOL_EXPOSURE_INTERNAL_H
#define MCP_TOOL_EXPOSURE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor_codec.h"
#include "device_schema.h"
#include "esp_err.h"
#include "mcp_tool_exposure.h"

#define MCP_EXP_NVS_NAMESPACE "mcp_exp"
#define MCP_EXP_NVS_KEY       "catalog"
#define MCP_EXP_STORE_SCHEMA_VERSION 3
#define MCP_EXP_NAMING_VERSION 3

/* Persisted record flags (bitfield in flags byte). */
#define MCP_EXP_FLAG_FEATURE_BOUND  (1u << 0)
#define MCP_EXP_FLAG_USER_DISABLED  (1u << 1)

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    uint8_t state;
    uint8_t reason;
    uint8_t naming_version;
    uint8_t flags;             /* MCP_EXP_FLAG_FEATURE_BOUND = hidden from catalog */
    char feature_id[GW_FEATURE_ID_LEN];
    uint8_t capability_digest[MCP_CAPABILITY_DIGEST_LEN];
} mcp_exposure_persisted_record_t;

typedef struct {
    uint8_t schema_version;
    uint8_t reserved0;
    uint16_t count;
    uint32_t catalog_revision;
    mcp_exposure_persisted_record_t records[];
} mcp_exposure_store_blob_t;

/* SHA-256 semantic digest (mcp_tool_digest.c) */
void mcp_tool_digest_compute(const device_schema_tool_t *cap,
                             uint8_t out[MCP_CAPABILITY_DIGEST_LEN]);

bool mcp_tool_digest_match(const uint8_t a[MCP_CAPABILITY_DIGEST_LEN],
                           const uint8_t b[MCP_CAPABILITY_DIGEST_LEN]);

/* NVS persistence (mcp_tool_exposure_store.c) */
esp_err_t mcp_exposure_store_load(mcp_exposure_persisted_record_t *records,
                                  size_t capacity, size_t *out_count,
                                  uint32_t *out_revision);
esp_err_t mcp_exposure_store_save(const mcp_exposure_persisted_record_t *records,
                                  size_t count, uint32_t revision);
esp_err_t mcp_exposure_store_erase(void);

#endif /* MCP_TOOL_EXPOSURE_INTERNAL_H */
