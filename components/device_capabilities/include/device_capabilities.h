#ifndef DEVICE_CAPABILITIES_H
#define DEVICE_CAPABILITIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor_codec.h"
#include "esp_err.h"

#define DEVICE_CAP_MAX_PER_DEVICE 12

#define DEVICE_CAP_RESERVED_COMMAND "describe_capabilities"

typedef enum {
    DEVICE_CAP_VALUE_NONE = 0,
    DEVICE_CAP_VALUE_BOOL = 1,
    DEVICE_CAP_VALUE_INT = 2,
} device_cap_value_type_t;

enum {
    DEVICE_CAP_FLAG_IDEMPOTENT = 1u << 0,
    DEVICE_CAP_FLAG_DESTRUCTIVE = 1u << 1,
};

typedef struct {
    char command[GW_MSG_COMMAND_LEN];
    char label[GW_MSG_CAP_LABEL_LEN];
    char unit[GW_MSG_CAP_UNIT_LEN];
    device_cap_value_type_t value_type;
    uint8_t flags;
    int32_t min_value;
    int32_t max_value;
    uint32_t step;
} device_capability_t;

/* Cache state — no STALE in target model. */
typedef enum {
    DEVICE_CAP_STATE_UNKNOWN = 0,
    DEVICE_CAP_STATE_DISCOVERING,
    DEVICE_CAP_STATE_READY,
    DEVICE_CAP_STATE_UNSUPPORTED,
    DEVICE_CAP_STATE_ERROR,
} device_cap_state_t;

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    device_cap_state_t state;
    /* True when a committed snapshot exists (loaded from NVS or committed
     * live). `state` alone is not authoritative: it reads DISCOVERING while a
     * refresh is in flight and stays DISCOVERING after a failed refresh when
     * a committed snapshot exists. */
    bool has_committed;
    uint32_t revision;
    uint32_t snapshot_id;
    int64_t updated_at_ms;
    size_t count;
    device_capability_t items[DEVICE_CAP_MAX_PER_DEVICE];
} device_capability_snapshot_t;

/* Refresh result for manual refresh reporting. */
typedef enum {
    DEVICE_CAP_REFRESH_RESULT_NONE = 0,
    DEVICE_CAP_REFRESH_RESULT_SUCCESS,
    DEVICE_CAP_REFRESH_RESULT_UNCHANGED,
    DEVICE_CAP_REFRESH_RESULT_NOT_PERSISTED,
    DEVICE_CAP_REFRESH_RESULT_UNSUPPORTED,
    DEVICE_CAP_REFRESH_RESULT_BUSY,
    DEVICE_CAP_REFRESH_RESULT_TIMEOUT,
    DEVICE_CAP_REFRESH_RESULT_DISCONNECTED,
    DEVICE_CAP_REFRESH_RESULT_TRANSPORT_ERROR,
    DEVICE_CAP_REFRESH_RESULT_PROTOCOL_ERROR,
    DEVICE_CAP_REFRESH_RESULT_INTERNAL_ERROR,
} device_cap_refresh_result_t;

/* Refresh active state — no COMPLETE; completion goes to last_completed. */
typedef enum {
    DEVICE_CAP_REFRESH_IDLE = 0,
    DEVICE_CAP_REFRESH_QUEUED,
    DEVICE_CAP_REFRESH_RUNNING,
} device_cap_refresh_state_t;

typedef struct {
    uint32_t generation;
    device_cap_refresh_state_t state;
} device_cap_refresh_active_t;

typedef struct {
    uint32_t generation;
    device_cap_refresh_result_t result;
    int64_t finished_at_ms;
} device_cap_refresh_completed_t;

/* Submit results for capability discovery/refresh submission. */
typedef enum {
    DEVICE_CAP_SUBMIT_OK = 0,
    DEVICE_CAP_SUBMIT_REJECTED,
    DEVICE_CAP_SUBMIT_TIMEOUT,
    DEVICE_CAP_SUBMIT_ERROR,
    DEVICE_CAP_SUBMIT_BUSY,
    DEVICE_CAP_SUBMIT_NOT_CONNECTED,
    DEVICE_CAP_SUBMIT_TRANSPORT_ERROR,
    DEVICE_CAP_SUBMIT_INTERNAL_ERROR,
} device_cap_submit_result_t;

typedef void (*device_cap_submit_done_fn)(device_cap_submit_result_t result,
                                          void *context);
typedef esp_err_t (*device_cap_submit_fn)(const gw_message_t *message,
                                          device_cap_submit_done_fn done,
                                          void *context);

typedef enum {
    DEVICE_CAP_VALID = 0,
    DEVICE_CAP_VALID_UNKNOWN,
    DEVICE_CAP_VALID_UNSUPPORTED_COMMAND,
    DEVICE_CAP_VALID_ARGUMENT,
} device_cap_validation_t;

esp_err_t device_capabilities_init(void);
void device_capabilities_set_submitter(device_cap_submit_fn submitter);

/* Commit listener — invoked from the capability worker after a snapshot is
 * committed (in memory) and the persist attempt has been made, always outside
 * the capability mutex. Listeners must be non-blocking and only enqueue work
 * (never call back into device_capabilities or wait on locks). Only one
 * listener slot; registering replaces the previous listener. */
typedef void (*device_capability_commit_listener_t)(const char *device_id,
                                                    uint32_t revision,
                                                    void *context);
esp_err_t device_capabilities_register_commit_listener(
    device_capability_commit_listener_t listener, void *context);

/* BLE lifecycle inputs. Both are non-blocking and safe from NimBLE callbacks. */
esp_err_t device_capabilities_on_ready(const char *device_id);
void device_capabilities_on_disconnect(const char *device_id);

/* Returns true when a capability begin/item/end message was recognized and
 * consumed. The function only queues a copy; validation and NVS happen on the
 * component worker. */
bool device_capabilities_on_notify(const char *device_id,
                                   const gw_message_t *message);

/* Queue health counters (Plan v1.1 §10.7). */
typedef struct {
    uint32_t enqueued;
    uint32_t dropped;
    uint32_t high_watermark;
    uint32_t message_alloc_fail;
} device_cap_queue_stats_t;

void device_capabilities_get_queue_stats(device_cap_queue_stats_t *out);

/* Manual refresh. Caller MUST preflight BLE ready status.
 * On success, *out_generation is set to the reserved generation. */
esp_err_t device_capabilities_refresh(const char *device_id,
                                      uint32_t *out_generation);

esp_err_t device_capabilities_get(const char *device_id,
                                  device_capability_snapshot_t *out_snapshot);

/* Copy out refresh status for a device. */
esp_err_t device_capabilities_get_refresh_status(
    const char *device_id,
    device_cap_refresh_active_t *out_active,
    device_cap_refresh_completed_t *out_completed);

device_cap_validation_t device_capabilities_validate_command(
    const gw_message_t *message, device_capability_t *out_capability);

/* Failure-safe: persistent erase first, then RAM clear. */
esp_err_t device_capabilities_forget(const char *device_id);

const char *device_capabilities_state_name(device_cap_state_t state);
const char *device_capabilities_refresh_result_name(
    device_cap_refresh_result_t result);

/* Test-only reset. It does not erase NVS. */
void device_capabilities_reset_for_test(void);

#endif // DEVICE_CAPABILITIES_H
