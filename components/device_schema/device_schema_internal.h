#ifndef DEVICE_SCHEMA_INTERNAL_H
#define DEVICE_SCHEMA_INTERNAL_H

#include "device_schema.h"

/* ── Operation enums ─────────────────────────────────────────────────── */

typedef enum {
    SCHEMA_OP_NONE    = 0,
    SCHEMA_OP_INITIAL = 1,
    SCHEMA_OP_MANUAL  = 2,
} schema_operation_kind_t;

typedef enum {
    SCHEMA_OP_IDLE    = 0,
    SCHEMA_OP_QUEUED  = 1,
    SCHEMA_OP_RUNNING = 2,
} schema_operation_state_t;

/* ── Per-device record ───────────────────────────────────────────────── */

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

    schema_operation_kind_t operation_kind;
    schema_operation_state_t operation_state;
    uint32_t operation_id;

    device_schema_refresh_active_t refresh_active;
    device_schema_refresh_completed_t refresh_last_completed;
} schema_record_t;

/* ── Runtime (device_schema_runtime.c) ──────────────────────────────── */

bool schema_runtime_lock(void);
void schema_runtime_unlock(void);

schema_record_t *schema_runtime_find_locked(const char *device_id);
schema_record_t *schema_runtime_find_or_create_locked(const char *device_id);

uint32_t schema_runtime_next_operation_id(void);
uint32_t schema_runtime_next_generation(void);

device_schema_submit_fn schema_runtime_get_submitter(void);

/* Owner — opaque to other modules, accessed via pointer. */
typedef struct {
    bool active;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t operation_id;
    schema_operation_kind_t kind;
    uint32_t refresh_generation;
} schema_global_owner_t;

schema_global_owner_t *schema_runtime_owner(void);

schema_record_t *schema_runtime_records(void);

bool schema_runtime_is_initialized(void);
void schema_runtime_set_initialized(bool v);

esp_err_t schema_runtime_init(void);
void schema_runtime_set_submitter(device_schema_submit_fn submitter);
void schema_runtime_set_commit_listener(device_schema_commit_listener_t listener,
                                        void *context);
void schema_runtime_set_commit_listener2(device_schema_commit_listener2_t listener,
                                         void *context);
void schema_runtime_notify_commit(const char *device_id, uint32_t revision);
void schema_runtime_reset(void);

/* ── Protocol (device_schema_protocol.c) ────────────────────────────── */

void schema_protocol_handle_message(const char *device_id,
                                    const gw_message_t *message);

/* ── Worker event types (shared with discovery/protocol) ────────────── */

typedef enum {
    SCHEMA_EVENT_READY = 0,
    SCHEMA_EVENT_REFRESH,
    SCHEMA_EVENT_DISCONNECT,
    SCHEMA_EVENT_NOTIFY,
    SCHEMA_EVENT_COMPLETION,
} schema_event_type_t;

typedef struct {
    schema_event_type_t type;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t operation_id;
    uint32_t refresh_generation;
    gw_message_t *message;
    device_schema_submit_result_t completion;
} schema_event_t;

/* ── Worker (device_schema_worker.c) ───────────────────────────────── */

esp_err_t schema_worker_init(void);
void schema_worker_reset_for_test(void);
esp_err_t schema_worker_post_event(const schema_event_t *event);
bool schema_worker_post_notify(const char *device_id,
                               const gw_message_t *message);
void schema_worker_get_stats(device_schema_queue_stats_t *out);

/* ── Discovery (device_schema_discovery.c) ──────────────────────────── */

void schema_discovery_handle_ready(const char *device_id);
void schema_discovery_start(const char *device_id,
                            schema_operation_kind_t kind,
                            uint32_t generation);
void schema_discovery_handle_disconnect(const char *device_id);
void schema_discovery_handle_completion(const char *device_id,
                                        uint32_t event_op_id,
                                        uint32_t event_generation,
                                        device_schema_submit_result_t completion);

/* ── Validation helpers (device_schema_validate.c) ──────────────────── */
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
void schema_cleanup_legacy_caps(void);

#endif /* DEVICE_SCHEMA_INTERNAL_H */
