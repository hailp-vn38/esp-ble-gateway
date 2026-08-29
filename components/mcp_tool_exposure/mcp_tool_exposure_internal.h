#ifndef MCP_TOOL_EXPOSURE_INTERNAL_H
#define MCP_TOOL_EXPOSURE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor_codec.h"
#include "device_capabilities.h"
#include "esp_err.h"
#include "mcp_tool_exposure.h"

#define MCP_EXP_NVS_NAMESPACE "mcp_exp"
#define MCP_EXP_NVS_KEY       "catalog"
#define MCP_EXP_STORE_SCHEMA_VERSION 2
#define MCP_EXP_NAMING_VERSION 2

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    uint8_t state;
    uint8_t reason;
    uint8_t naming_version;
    uint8_t reserved;
    uint8_t capability_digest[MCP_CAPABILITY_DIGEST_LEN];
} mcp_exposure_persisted_record_t;

typedef struct {
    uint8_t schema_version;
    uint8_t reserved0;
    uint16_t count;
    uint32_t catalog_revision;
    mcp_exposure_persisted_record_t records[];
} mcp_exposure_store_blob_t;

/* Tool name generation (mcp_tool_name.c) */
esp_err_t mcp_tool_name_generate(const char *device_name, const char *command,
                                 char *out_name, size_t out_len);

/* SHA-256 semantic digest (mcp_tool_digest.c) */
void mcp_tool_digest_compute(const device_capability_t *cap,
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

/* Dynamic catalog (mcp_tool_catalog.c) */
typedef void (*mcp_catalog_change_fn)(uint32_t revision, void *context);

esp_err_t mcp_tool_catalog_init(mcp_catalog_change_fn on_change,
                                void *change_context);
esp_err_t mcp_tool_catalog_add(const mcp_tool_binding_t *binding);
esp_err_t mcp_tool_catalog_remove(const char *tool_name);
const mcp_tool_binding_t *mcp_tool_catalog_find_ptr(const char *tool_name);
uint32_t mcp_tool_catalog_get_revision(void);
void mcp_tool_catalog_get_snapshot(mcp_tool_binding_t *out,
                                  size_t capacity, size_t *out_count);
void mcp_tool_catalog_remove_device(const char *device_id);

#endif /* MCP_TOOL_EXPOSURE_INTERNAL_H */
