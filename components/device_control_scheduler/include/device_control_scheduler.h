#ifndef DEVICE_CONTROL_SCHEDULER_H
#define DEVICE_CONTROL_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include "device_command_types.h"
#include "esp_err.h"

typedef uint32_t device_control_job_id_t;
typedef uint32_t device_control_owner_token_t;
typedef uint32_t device_control_lease_t;

typedef enum { DEVICE_CTRL_PRIORITY_HIGH = 0, DEVICE_CTRL_PRIORITY_NORMAL,
               DEVICE_CTRL_PRIORITY_LOW, DEVICE_CTRL_PRIORITY_BACKGROUND } device_control_priority_t;
typedef enum { DEVICE_CTRL_SOURCE_WEB = 0, DEVICE_CTRL_SOURCE_MCP,
               DEVICE_CTRL_SOURCE_SCHEMA, DEVICE_CTRL_SOURCE_SETTINGS,
               DEVICE_CTRL_SOURCE_STATE, DEVICE_CTRL_SOURCE_INTERNAL } device_control_source_t;
typedef enum { DEVICE_CTRL_DEDUPE_NONE = 0, DEVICE_CTRL_DEDUPE_DROP_IF_EXISTS,
               DEVICE_CTRL_DEDUPE_REPLACE_QUEUED } device_control_dedupe_t;
typedef enum { DEVICE_CTRL_TERMINAL_COMMAND = 0, DEVICE_CTRL_TERMINAL_CANCELLED,
               DEVICE_CTRL_TERMINAL_SUPERSEDED, DEVICE_CTRL_TERMINAL_DEADLINE_EXCEEDED } device_control_terminal_t;

typedef struct {
    device_command_request_t request;
    device_control_priority_t priority;
    device_control_source_t source;
    device_control_dedupe_t dedupe;
    device_control_owner_token_t owner_token;
    device_control_lease_t lease_id;
    uint32_t max_queue_wait_ms;
    uint32_t dedupe_hash;
} device_control_job_t;

typedef struct {
    device_control_job_id_t scheduler_job_id;
    device_control_terminal_t terminal;
    device_command_result_t command_result;
} device_control_result_t;
typedef void (*device_control_completion_fn)(const device_control_result_t *result, void *context);

typedef struct {
    const char *device_id;
    device_control_priority_t priority;
    device_control_source_t source;
    device_control_owner_token_t owner_token;
} device_control_lease_request_t;
typedef void (*device_control_lease_completion_fn)(esp_err_t status,
                                                     device_control_lease_t lease_id,
                                                     void *context);

typedef struct {
    uint32_t submitted, dispatched, completed, cancelled, superseded, deadline_exceeded;
    uint32_t queue_full, deduped, max_queued, max_inflight;
    uint32_t lease_granted, lease_revoked, max_active_leases, lease_hold_max_ms;
    uint32_t dcs_busy, completion_mailbox_conflict;
} device_control_scheduler_stats_t;

esp_err_t device_control_scheduler_init(void);
void device_control_scheduler_deinit(void);
esp_err_t device_control_scheduler_submit(const device_control_job_t *job,
                                           device_control_completion_fn completion,
                                           void *context,
                                           device_control_job_id_t *out_job_id);
esp_err_t device_control_scheduler_cancel_job(device_control_job_id_t job_id);
esp_err_t device_control_scheduler_cancel_source(const char *device_id,
                                                  device_control_source_t source,
                                                  device_control_owner_token_t owner_token);
esp_err_t device_control_scheduler_cancel_device(const char *device_id);
esp_err_t device_control_scheduler_acquire_lease(const device_control_lease_request_t *request,
                                                  device_control_lease_completion_fn completion,
                                                  void *context);
esp_err_t device_control_scheduler_release_lease(device_control_lease_t lease_id);
esp_err_t device_control_scheduler_block_device(const char *device_id);
esp_err_t device_control_scheduler_unblock_device(const char *device_id);
esp_err_t device_control_scheduler_quiesce_device(const char *device_id, uint32_t timeout_ms);
bool device_control_scheduler_is_idle(const char *device_id);
void device_control_scheduler_get_stats(device_control_scheduler_stats_t *out);

#endif
