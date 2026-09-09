#ifndef DEVICE_COMMAND_SERVICE_INTERNAL_H
#define DEVICE_COMMAND_SERVICE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_command_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DCS_QUEUE_LEN                8
#define DCS_URGENT_QUEUE_LEN         8
#define DCS_TASK_STACK            5120
#define DCS_TASK_PRIORITY         (tskIDLE_PRIORITY + 4)
#define DCS_MAX_PENDING DEVICE_COMMAND_SERVICE_MAX_PENDING
#define DCS_ACK_TIMEOUT_MS        2000
#define DCS_DEINIT_WAIT_BUDGET_MS 5000

typedef enum {
    DCS_URGENT_EVENT_ACK = 0,
    DCS_URGENT_EVENT_DISCONNECT,
    DCS_URGENT_EVENT_CANCEL,
    DCS_URGENT_EVENT_SHUTDOWN,
} dcs_urgent_event_type_t;

typedef struct {
    device_command_request_t request;
    device_command_completion_fn completion;
    void *context;
} dcs_submit_event_t;

typedef struct {
    device_id_t device_id;
    uint32_t request_id;
    device_command_t command;
    bool accepted;
    bool has_bool_value;
    bool bool_value;
    bool has_int_value;
    int32_t int_value;
    bool has_feature_value_bool;
    bool feature_value_bool;
    bool has_feature_value_int;
    int32_t feature_value_int;
} dcs_ack_event_t;

typedef struct {
    dcs_urgent_event_type_t type;
    int64_t enqueued_us;
    union {
        dcs_ack_event_t ack;
        device_id_t device_id;
    } data;
} dcs_urgent_event_t;

_Static_assert(sizeof(dcs_ack_event_t) < sizeof(gw_message_t),
               "urgent ACK must remain compact");

typedef struct {
    bool in_use;
    uint32_t request_id;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    device_command_origin_t origin;
    device_command_completion_fn completion;
    void *context;
    int64_t deadline_us;
    bool has_bool_value;
    bool bool_value;
    bool has_int_value;
    int32_t int_value;
    bool has_feature_id;
    char feature_id[GW_FEATURE_ID_LEN];
    bool has_property_id;
    uint8_t property_id;
} dcs_pending_slot_t;

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t request_id;
    char command[GW_MSG_COMMAND_LEN];
} dcs_completed_ack_t;

typedef struct {
    QueueHandle_t normal_queue;
    QueueHandle_t urgent_queue;
    TaskHandle_t task;
    volatile bool running;
    volatile bool task_stopped;
    dcs_pending_slot_t pending[DCS_MAX_PENDING];
    dcs_completed_ack_t completed_acks[DCS_MAX_PENDING];
    uint32_t completed_ack_next;
    uint32_t next_request_id;
    device_command_service_stats_t stats;
    portMUX_TYPE stats_mux;
    device_command_transport_hooks_t hooks;
} dcs_state_t;

extern dcs_state_t g_dcs;
extern const char *DCS_TAG;

void dcs_stats_inc(uint32_t *field);
device_command_status_t dcs_validate_request(const device_command_request_t *request,
                                             const gw_message_t *wire_message);
void dcs_build_wire_message(const device_command_request_t *request,
                            uint32_t request_id, gw_message_t *message);
void dcs_pending_reset(void);
dcs_pending_slot_t *dcs_pending_find_device(const char *device_id);
dcs_pending_slot_t *dcs_pending_find_id(const char *device_id, uint32_t request_id);
dcs_pending_slot_t *dcs_pending_allocate(void);
uint32_t dcs_pending_next_request_id(void);
uint32_t dcs_pending_count(void);
void dcs_pending_complete(dcs_pending_slot_t *slot,
                          const device_command_result_t *result);
void dcs_pending_complete_status(dcs_pending_slot_t *slot,
                                 device_command_status_t status);
void dcs_pending_note_completed_ack(const dcs_pending_slot_t *slot);
bool dcs_pending_is_duplicate_ack(const dcs_ack_event_t *ack);
bool dcs_urgent_enqueue(const dcs_urgent_event_t *event, TickType_t wait_ticks);
void dcs_urgent_drain(void);
void dcs_handle_submit(const dcs_submit_event_t *event);
void dcs_service_task(void *arg);

#endif
