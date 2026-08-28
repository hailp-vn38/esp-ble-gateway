#ifndef MCP_TOOL_EXPOSURE_H
#define MCP_TOOL_EXPOSURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_capabilities.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_DYNAMIC_TOOL_NAME_MAX 128
#define MCP_CAPABILITY_DIGEST_LEN 16

typedef enum {
    MCP_EXPOSURE_ENABLED = 0,
    MCP_EXPOSURE_NEEDS_REVIEW,
    MCP_EXPOSURE_ORPHANED,
} mcp_exposure_state_t;

typedef enum {
    MCP_EXPOSURE_REASON_NONE = 0,
    MCP_EXPOSURE_REASON_CAPABILITY_CHANGED,
    MCP_EXPOSURE_REASON_COMMAND_MISSING,
    MCP_EXPOSURE_REASON_DEVICE_MISSING,
    MCP_EXPOSURE_REASON_POLICY_BLOCKED,
    MCP_EXPOSURE_REASON_PERSIST_DIRTY,
} mcp_exposure_reason_t;

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
    mcp_exposure_state_t state;
    mcp_exposure_reason_t reason;
    uint8_t capability_digest[MCP_CAPABILITY_DIGEST_LEN];
} mcp_tool_exposure_t;

typedef struct {
    char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    device_capability_t capability;
} mcp_tool_binding_t;

typedef struct {
    bool confirm_destructive;
} mcp_exposure_enable_options_t;

typedef struct {
    size_t enabled;
    size_t max_enabled;
    size_t records;
    size_t max_records;
} mcp_exposure_capacity_t;

esp_err_t mcp_tool_exposure_init(void);

esp_err_t mcp_tool_exposure_enable(
    const char *device_id,
    const char *command,
    const mcp_exposure_enable_options_t *options);

esp_err_t mcp_tool_exposure_disable(
    const char *device_id,
    const char *command);

esp_err_t mcp_tool_exposure_get(
    const char *device_id,
    const char *command,
    mcp_tool_exposure_t *out);

esp_err_t mcp_tool_exposure_snapshot(
    mcp_tool_exposure_t *out,
    size_t capacity,
    size_t *out_count);

const mcp_tool_binding_t *mcp_tool_catalog_find_ptr(const char *tool_name);

void mcp_tool_catalog_get_snapshot(mcp_tool_binding_t *out,
                                   size_t capacity, size_t *out_count);

uint32_t mcp_tool_catalog_get_revision(void);

esp_err_t mcp_tool_exposure_reconcile_device_async(
    const char *device_id,
    uint32_t capability_revision);

esp_err_t mcp_tool_exposure_forget_device(const char *device_id);

esp_err_t mcp_tool_exposure_get_capacity(mcp_exposure_capacity_t *out);

// Tool naming (mcp_tool_name.c).
esp_err_t mcp_tool_name_generate(const char *device_id, const char *command,
                                 char *out, size_t out_len);

// Capability digest (mcp_tool_digest.c).
void mcp_tool_digest_compute(const device_capability_t *cap,
                             uint8_t out[MCP_CAPABILITY_DIGEST_LEN]);
bool mcp_tool_digest_match(const uint8_t a[MCP_CAPABILITY_DIGEST_LEN],
                           const uint8_t b[MCP_CAPABILITY_DIGEST_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* MCP_TOOL_EXPOSURE_H */
