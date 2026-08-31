#ifndef DEVICE_SCHEMA_H
#define DEVICE_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor_codec.h"
#include "esp_err.h"

#define DEVICE_SCHEMA_MAX_TOOLS     12
#define DEVICE_SCHEMA_MAX_FEATURES  12
#define DEVICE_SCHEMA_RESERVED_COMMAND "describe_capabilities"

/* ── Tool (command capability) ──────────────────────────────────────── */

typedef struct {
    char command[GW_MSG_COMMAND_LEN];
    char label[GW_MSG_CAP_LABEL_LEN];
    char unit[GW_MSG_CAP_UNIT_LEN];
    uint8_t value_type;   /* gw_feature_property_t range */
    uint8_t flags;
    int32_t min_value;
    int32_t max_value;
    uint32_t step;
} device_schema_tool_t;

enum {
    DEVICE_SCHEMA_FLAG_IDEMPOTENT  = 1u << 0,
    DEVICE_SCHEMA_FLAG_DESTRUCTIVE = 1u << 1,
};

/* ── Feature ────────────────────────────────────────────────────────── */

typedef struct {
    char feature_id[GW_FEATURE_ID_LEN];
    uint8_t feature_type;
    uint16_t feature_schema_version;
    uint16_t feature_flags;
    uint8_t property_id;
    bool feature_value_bool;
    int32_t feature_value_int;
    int8_t writable_tool_index; /* index into tools[], -1 = none */
} device_schema_feature_t;

/* ── Schema state ───────────────────────────────────────────────────── */

typedef enum {
    DEVICE_SCHEMA_STATE_UNKNOWN = 0,
    DEVICE_SCHEMA_STATE_DISCOVERING,
    DEVICE_SCHEMA_STATE_READY,
    DEVICE_SCHEMA_STATE_UNSUPPORTED,
    DEVICE_SCHEMA_STATE_ERROR,
} device_schema_state_t;

/* ── Committed schema snapshot ──────────────────────────────────────── */

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    device_schema_state_t state;
    bool has_committed;
    uint32_t revision;
    uint32_t snapshot_id;
    int64_t updated_at_ms;
    size_t tool_count;
    size_t feature_count;
    device_schema_tool_t tools[DEVICE_SCHEMA_MAX_TOOLS];
    device_schema_feature_t features[DEVICE_SCHEMA_MAX_FEATURES];
} device_schema_snapshot_t;

/* ── Validation result ──────────────────────────────────────────────── */

typedef enum {
    DEVICE_SCHEMA_VALID = 0,
    DEVICE_SCHEMA_VALID_UNKNOWN,
    DEVICE_SCHEMA_VALID_UNSUPPORTED_COMMAND,
    DEVICE_SCHEMA_VALID_ARGUMENT,
} device_schema_validation_t;

/* ── Submit result ──────────────────────────────────────────────────── */

typedef enum {
    DEVICE_SCHEMA_SUBMIT_OK = 0,
    DEVICE_SCHEMA_SUBMIT_REJECTED,
    DEVICE_SCHEMA_SUBMIT_TIMEOUT,
    DEVICE_SCHEMA_SUBMIT_ERROR,
    DEVICE_SCHEMA_SUBMIT_BUSY,
    DEVICE_SCHEMA_SUBMIT_NOT_CONNECTED,
    DEVICE_SCHEMA_SUBMIT_TRANSPORT_ERROR,
    DEVICE_SCHEMA_SUBMIT_INTERNAL_ERROR,
} device_schema_submit_result_t;

typedef void (*device_schema_submit_done_fn)(device_schema_submit_result_t result,
                                             void *context);
typedef esp_err_t (*device_schema_submit_fn)(const gw_message_t *message,
                                             device_schema_submit_done_fn done,
                                             void *context);

/* ── Refresh status ─────────────────────────────────────────────────── */

typedef enum {
    DEVICE_SCHEMA_REFRESH_RESULT_NONE = 0,
    DEVICE_SCHEMA_REFRESH_RESULT_SUCCESS,
    DEVICE_SCHEMA_REFRESH_RESULT_UNCHANGED,
    DEVICE_SCHEMA_REFRESH_RESULT_NOT_PERSISTED,
    DEVICE_SCHEMA_REFRESH_RESULT_UNSUPPORTED,
    DEVICE_SCHEMA_REFRESH_RESULT_BUSY,
    DEVICE_SCHEMA_REFRESH_RESULT_TIMEOUT,
    DEVICE_SCHEMA_REFRESH_RESULT_DISCONNECTED,
    DEVICE_SCHEMA_REFRESH_RESULT_TRANSPORT_ERROR,
    DEVICE_SCHEMA_REFRESH_RESULT_PROTOCOL_ERROR,
    DEVICE_SCHEMA_REFRESH_RESULT_INTERNAL_ERROR,
} device_schema_refresh_result_t;

typedef enum {
    DEVICE_SCHEMA_REFRESH_IDLE = 0,
    DEVICE_SCHEMA_REFRESH_QUEUED,
    DEVICE_SCHEMA_REFRESH_RUNNING,
} device_schema_refresh_state_t;

typedef struct {
    uint32_t generation;
    device_schema_refresh_state_t state;
} device_schema_refresh_active_t;

typedef struct {
    uint32_t generation;
    device_schema_refresh_result_t result;
    int64_t finished_at_ms;
} device_schema_refresh_completed_t;

/* ── Queue stats ────────────────────────────────────────────────────── */

typedef struct {
    uint32_t enqueued;
    uint32_t dropped;
    uint32_t high_watermark;
    uint32_t message_alloc_fail;
} device_schema_queue_stats_t;

/* ── Commit listener ────────────────────────────────────────────────── */

typedef void (*device_schema_commit_listener_t)(const char *device_id,
                                                 uint32_t revision,
                                                 void *context);

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t device_schema_init(void);
void device_schema_set_submitter(device_schema_submit_fn submitter);

esp_err_t device_schema_register_commit_listener(
    device_schema_commit_listener_t listener, void *context);

esp_err_t device_schema_on_ready(const char *device_id);
void device_schema_on_disconnect(const char *device_id);

bool device_schema_on_notify(const char *device_id,
                              const gw_message_t *message);

void device_schema_get_queue_stats(device_schema_queue_stats_t *out);

esp_err_t device_schema_refresh(const char *device_id,
                                 uint32_t *out_generation);

esp_err_t device_schema_get(const char *device_id,
                             device_schema_snapshot_t *out_snapshot);

esp_err_t device_schema_get_refresh_status(
    const char *device_id,
    device_schema_refresh_active_t *out_active,
    device_schema_refresh_completed_t *out_completed);

device_schema_validation_t device_schema_validate_command(
    const gw_message_t *message, device_schema_tool_t *out_tool);

esp_err_t device_schema_forget(const char *device_id);

const char *device_schema_state_name(device_schema_state_t state);
const char *device_schema_refresh_result_name(
    device_schema_refresh_result_t result);

void device_schema_reset_for_test(void);

#endif /* DEVICE_SCHEMA_H */
