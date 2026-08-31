#ifndef DEVICE_SCHEMA_INTERNAL_H
#define DEVICE_SCHEMA_INTERNAL_H

#include "device_schema.h"

/* Per-device record used by the worker and store modules. */
typedef struct {
    bool used;
    bool has_committed;
    bool persist_dirty;

    device_schema_snapshot_t committed;

    bool staging_active;
    uint32_t staging_operation_id;
    uint16_t staging_expected_tools;
    uint16_t staging_expected_features;
    device_schema_snapshot_t staging;

    /* Operation tracking */
    int operation_kind;   /* 0=none, 1=initial, 2=manual */
    int operation_state;  /* 0=idle, 1=queued, 2=running */
    uint32_t operation_id;

    device_schema_refresh_active_t refresh_active;
    device_schema_refresh_completed_t refresh_last_completed;
} schema_record_t;

/* Validation helpers (device_schema_validate.c) */
bool schema_valid_command_name(const char *command);
bool schema_valid_tool(const device_schema_tool_t *tool);
bool schema_tool_equal(const device_schema_tool_t *a,
                       const device_schema_tool_t *b);
bool schema_valid_feature_id(const char *feature_id);
int8_t schema_resolve_writable_tool(const device_schema_tool_t *tools,
                                     size_t tool_count,
                                     const char *feature_tool);

/* Store functions (device_schema_store.c) */
esp_err_t schema_persist_record(int index,
                                const device_schema_snapshot_t *snapshot);
void schema_load_persisted(schema_record_t *records);
esp_err_t schema_erase_nvs(int index);

#endif /* DEVICE_SCHEMA_INTERNAL_H */
