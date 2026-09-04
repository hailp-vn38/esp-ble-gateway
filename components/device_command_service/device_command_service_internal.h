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
#define DCS_TASK_STACK            3072
#define DCS_TASK_PRIORITY         (tskIDLE_PRIORITY + 4)
#define DCS_MAX_PENDING              4
#define DCS_ACK_TIMEOUT_MS        2000
#define DCS_DEINIT_WAIT_BUDGET_MS 5000

typedef enum {
    DCS_EVENT_SUBMIT = 0,
    DCS_EVENT_ACK,
    DCS_EVENT_DISCONNECT,
    DCS_EVENT_CANCEL,
    DCS_EVENT_SHUTDOWN,
} dcs_event_type_t;

typedef struct {
    dcs_event_type_t type;
    device_command_request_t request;
    device_command_completion_fn completion;
    void *context;
    char ack_device_id[GW_MSG_DEVICE_ID_LEN];
    gw_message_t ack_message;
    char disconnect_device_id[GW_MSG_DEVICE_ID_LEN];
    char cancel_device_id[GW_MSG_DEVICE_ID_LEN];
} dcs_event_t;

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
    QueueHandle_t queue;
    TaskHandle_t task;
    volatile bool running;
    volatile bool task_stopped;
    dcs_pending_slot_t pending[DCS_MAX_PENDING];
    uint32_t next_request_id;
    device_command_service_stats_t stats;
    portMUX_TYPE stats_mux;
    device_command_transport_hooks_t hooks;
} dcs_state_t;

extern dcs_state_t g_dcs;
extern const char *DCS_TAG;

void dcs_stats_inc(uint32_t *field);
device_command_status_t dcs_validate_request(const device_command_request_t *request);
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
void dcs_service_task(void *arg);

#endif
