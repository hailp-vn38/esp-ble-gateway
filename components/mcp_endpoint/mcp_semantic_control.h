#ifndef MCP_SEMANTIC_CONTROL_H
#define MCP_SEMANTIC_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "device_schema.h"

/* ── Resolve status ────────────────────────────────────────────────── */

typedef enum {
    MCP_SEM_OK = 0,
    MCP_SEM_NOT_FOUND,
    MCP_SEM_AMBIGUOUS,
    MCP_SEM_INVALID,
} mcp_sem_resolve_status_t;

/* ── Device resolution ─────────────────────────────────────────────── */

/**
 * Resolve a device identifier to a device_id.
 * Exact device_id match is preferred. Falls back to unique configured
 * name. Returns AMBIGUOUS if multiple devices share the same name.
 */
mcp_sem_resolve_status_t mcp_sem_resolve_device(const cJSON *arg,
                                                 char *out, size_t out_len);

/* ── Feature resolution ────────────────────────────────────────────── */

/**
 * Resolve a feature identifier against a committed schema.
 * Exact feature_id match is preferred. Falls back to unique semantic
 * name from device_template. Returns AMBIGUOUS if ambiguous.
 */
mcp_sem_resolve_status_t mcp_sem_resolve_feature(
    const device_schema_snapshot_t *schema,
    const cJSON *arg,
    device_schema_feature_t *out);

/* ── Feature serialization ─────────────────────────────────────────── */

/**
 * Serialize one feature into a cJSON object and append to @p array.
 * Includes feature_id, semantic_name, type, property, value_type,
 * writable, and min/max/step for writable INT features.
 * Returns false on allocation failure.
 */
bool mcp_sem_serialize_feature(cJSON *array,
                               const device_schema_snapshot_t *schema,
                               const device_schema_feature_t *feature);

#endif /* MCP_SEMANTIC_CONTROL_H */
