#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "device_schema.h"
#include "device_store.h"
#include "device_template.h"

#include "mcp_endpoint_internal.h"

// MCP policy evaluation per spec §16.
// Sequence:
//   1. Tool exposed? (registry lookup — caller handles)
//   2. Device exists?
//   3. Capabilities ready?
//   4. Command advertised?
//   5. Global allow policy (allowlist)?
//   6. Destructive? -> deny for control profile

mcp_policy_result_t mcp_policy_check_device_command(const char *device_id,
                                                     const char *command)
{
    // 1. Device exists in store?
    device_entry_t entry;
    device_store_result_t store_result =
        device_store_get(device_id, &entry);
    if (store_result != DEVICE_STORE_OK) {
        return MCP_POLICY_DEVICE_UNAVAILABLE;
    }

    // 2. Schema advertisement check
    device_schema_snapshot_t snapshot;
    esp_err_t cap_err = device_schema_get(device_id, &snapshot);
    if (cap_err != ESP_OK) {
        // Schema not available — cannot verify command
        return MCP_POLICY_CAPABILITY_UNKNOWN;
    }

    if (snapshot.state != DEVICE_SCHEMA_STATE_READY) {
        return MCP_POLICY_CAPABILITY_UNKNOWN;
    }

    // 3. Command advertised?
    bool command_found = false;
    bool is_destructive = false;
    for (size_t i = 0; i < snapshot.tool_count; i++) {
        if (strcmp(snapshot.tools[i].command, command) == 0) {
            command_found = true;
            is_destructive =
                (snapshot.tools[i].flags & DEVICE_SCHEMA_FLAG_DESTRUCTIVE) != 0;
            break;
        }
    }

    if (!command_found) {
        return MCP_POLICY_CAPABILITY_UNKNOWN;
    }

    // 4. Global allow policy (allowlist check)
    if (!mcp_device_command_allowed(command)) {
        return MCP_POLICY_DENY_COMMAND;
    }

    // 5. Destructive guard: MCP control/voice profile denies destructive
    if (is_destructive) {
        return MCP_POLICY_DENY_DESTRUCTIVE;
    }

    return MCP_POLICY_ALLOW;
}

// ---------------------------------------------------------------------------
// Semantic feature-control policy (Phase 10)
// Centralized write-policy for compact semantic control. Replaces the raw
// command allowlist as the authority for semantic device control.
// ---------------------------------------------------------------------------

mcp_policy_result_t mcp_policy_check_feature_control(
    const char *device_id,
    const char *feature_id,
    const device_schema_tool_t *resolved_tool)
{
    if (device_id == NULL || feature_id == NULL || resolved_tool == NULL) {
        return MCP_POLICY_DENY_COMMAND;
    }

    /* 1. Device exists */
    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        return MCP_POLICY_DEVICE_UNAVAILABLE;
    }

    /* 2. Committed schema exists */
    device_schema_snapshot_t snapshot;
    if (device_schema_get(device_id, &snapshot) != ESP_OK ||
        !snapshot.has_committed) {
        return MCP_POLICY_CAPABILITY_UNKNOWN;
    }

    /* 3. Feature exists in committed schema */
    bool feature_found = false;
    device_schema_feature_t feature = {0};
    for (size_t i = 0; i < snapshot.feature_count; i++) {
        if (strcmp(snapshot.features[i].feature_id, feature_id) == 0) {
            feature_found = true;
            feature = snapshot.features[i];
            break;
        }
    }
    if (!feature_found) {
        return MCP_POLICY_CAPABILITY_UNKNOWN;
    }

    /* 4. writable_tool_index valid */
    if (feature.writable_tool_index < 0 ||
        (size_t)feature.writable_tool_index >= snapshot.tool_count) {
        return MCP_POLICY_DENY_COMMAND;
    }

    /* 5. A persisted semantic grant is mandatory. */
    mcp_tool_exposure_t exposure;
    if (mcp_tool_exposure_get_feature(device_id, feature_id,
                                      &exposure) != ESP_OK) {
        return MCP_POLICY_DENY_COMMAND;
    }
    if (!exposure.control_enabled) return MCP_POLICY_DENY_COMMAND;

    /* 6. Capability health must have been explicitly accepted. */
    if (exposure.state != MCP_EXPOSURE_ENABLED) {
        return MCP_POLICY_CAPABILITY_UNKNOWN;
    }

    /* 7. Digest matches */
    uint8_t current_digest[MCP_CAPABILITY_DIGEST_LEN];
    mcp_tool_digest_compute(resolved_tool, current_digest);
    if (!mcp_tool_digest_match(current_digest, exposure.capability_digest)) {
        return MCP_POLICY_CAPABILITY_UNKNOWN;
    }

    /* 8. Destructive grant allowed */
    if (resolved_tool->flags & DEVICE_SCHEMA_FLAG_DESTRUCTIVE) {
        return MCP_POLICY_DENY_DESTRUCTIVE;
    }

    return MCP_POLICY_ALLOW;
}
