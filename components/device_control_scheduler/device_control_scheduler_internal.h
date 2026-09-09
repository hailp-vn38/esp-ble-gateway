#ifndef DEVICE_CONTROL_SCHEDULER_INTERNAL_H
#define DEVICE_CONTROL_SCHEDULER_INTERNAL_H

#include "device_control_scheduler.h"
#include "device_command_service.h"
#include "device_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DEVICE_CTRL_MAX_DEVICES 16
#define DEVICE_CTRL_MAX_JOBS 24
#define DEVICE_CTRL_MAX_INFLIGHT 4
#define DEVICE_CTRL_TASK_STACK 4096
#define DEVICE_CTRL_TASK_PRIORITY (tskIDLE_PRIORITY + 4)
#define DEVICE_CTRL_HIGH_BURST_MAX 8

_Static_assert(DEVICE_CTRL_MAX_INFLIGHT <= DEVICE_COMMAND_SERVICE_MAX_PENDING, "scheduler exceeds DCS pending capacity");
_Static_assert(DEVICE_CTRL_MAX_DEVICES >= DEVICE_STORE_MAX_DEVICES, "scheduler device table too small");

typedef enum { CTRL_SLOT_FREE, CTRL_SLOT_ENQUEUE_PENDING, CTRL_SLOT_QUEUED,
               CTRL_SLOT_INFLIGHT, CTRL_SLOT_COMPLETION_PENDING, CTRL_SLOT_COMPLETING } ctrl_slot_state_t;
typedef struct { uint16_t slot; uint32_t generation; } ctrl_dcs_context_t;
typedef struct {
    ctrl_slot_state_t state; uint32_t generation; device_control_job_id_t id;
    device_control_job_t job; device_control_completion_fn completion; void *context;
    int64_t enqueue_us, deadline_us; device_command_result_t pending_result;
    ctrl_dcs_context_t dcs_context; device_control_terminal_t terminal;
    bool completion_written; int16_t device_index;
} ctrl_slot_t;
typedef struct { bool used, blocked, transport_inflight; device_id_t device_id;
    int16_t inflight_slot; device_control_lease_t lease; device_control_owner_token_t lease_owner;
    device_control_source_t lease_source; int64_t lease_started_us; } ctrl_device_t;
typedef enum { CTRL_LEASE_FREE, CTRL_LEASE_PENDING, CTRL_LEASE_ACTIVE,
               CTRL_LEASE_COMPLETION_PENDING } ctrl_lease_state_t;
typedef struct { ctrl_lease_state_t state; device_control_lease_t id; int16_t device_index;
    device_control_priority_t priority; device_control_source_t source; device_control_owner_token_t owner;
    device_control_lease_completion_fn completion; void *context; esp_err_t completion_status; } ctrl_lease_t;
typedef struct { TaskHandle_t task; volatile bool running, stopped; portMUX_TYPE mux;
    ctrl_slot_t slots[DEVICE_CTRL_MAX_JOBS]; ctrl_device_t devices[DEVICE_CTRL_MAX_DEVICES];
    ctrl_lease_t leases[DEVICE_CTRL_MAX_DEVICES]; uint32_t next_job, next_lease, inflight, rr_device, high_burst;
    device_control_scheduler_stats_t stats; } ctrl_state_t;
extern ctrl_state_t g_ctrl;
void ctrl_worker(void *arg); void ctrl_wake(void); void ctrl_process(void);
int16_t ctrl_find_device_locked(const char *id, bool create); void ctrl_stats_locked(void);
bool ctrl_job_equal(const device_control_job_t *a, const device_control_job_t *b);
void ctrl_complete_slot(uint16_t index); void ctrl_dcs_complete(const device_command_result_t *result, void *context);
void ctrl_process_leases(void);
#endif
