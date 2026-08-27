#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "device_capabilities.h"
#include "device_store.h"

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

    // 2. Capabilities advertisement check
    device_capability_snapshot_t snapshot;
    esp_err_t cap_err = device_capabilities_get(device_id, &snapshot);
    if (cap_err != ESP_OK) {
        // Capabilities not available — cannot verify command
        return MCP_POLICY_CAPABILITY_UNKNOWN;
    }

    if (snapshot.state != DEVICE_CAP_STATE_READY) {
        return MCP_POLICY_CAPABILITY_UNKNOWN;
    }

    // 3. Command advertised?
    bool command_found = false;
    bool is_destructive = false;
    for (size_t i = 0; i < snapshot.count; i++) {
        if (strcmp(snapshot.items[i].command, command) == 0) {
            command_found = true;
            is_destructive =
                (snapshot.items[i].flags & DEVICE_CAP_FLAG_DESTRUCTIVE) != 0;
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
