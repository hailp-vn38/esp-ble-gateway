#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "device_schema.h"
#include "device_store.h"
#include "device_template.h"

#include "mcp_endpoint_internal.h"

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
