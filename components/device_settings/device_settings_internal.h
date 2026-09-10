#ifndef DEVICE_SETTINGS_INTERNAL_H
#define DEVICE_SETTINGS_INTERNAL_H

#include "device_settings.h"
#include "device_control_scheduler.h"

typedef enum {
    DS_EVENT_START_OPERATION = 0,
    DS_EVENT_LEASE_RESULT,
    DS_EVENT_COMMAND_COMPLETE,
    DS_EVENT_VALUES_STREAM_COMPLETE,
    DS_EVENT_DISCONNECTED,
    DS_EVENT_RETRY_TIMER,
    DS_EVENT_CANCEL_DEVICE,
    DS_EVENT_TRANSACTION_START,
    DS_EVENT_TRANSACTION_ABORT,
    DS_EVENT_TRANSACTION_TIMEOUT,
    DS_EVENT_FORGET_DEVICE,
    DS_EVENT_SHUTDOWN,
} ds_event_type_t;

typedef struct {
    ds_event_type_t type;
    int16_t slot;
    uint32_t generation;
    device_control_owner_token_t owner_token;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    union {
        struct { esp_err_t status; device_control_lease_t id; } lease;
        device_command_result_t command;
        struct { bool success; } values;
    } data;
} ds_event_t;

typedef struct {
    uint32_t event_queue_full;
    uint32_t event_fallback;
    uint32_t stale_events;
    uint32_t lease_release_fail;
} ds_actor_metrics_t;

esp_err_t ds_events_init(void);
void ds_events_deinit(void);
bool ds_events_post(const ds_event_t *event);
bool ds_events_wait(ds_event_t *event);
void ds_events_get_metrics(ds_actor_metrics_t *out);
bool device_settings_worker_actor_running(void);

#endif
