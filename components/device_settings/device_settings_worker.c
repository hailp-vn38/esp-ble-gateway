/* Settings actor.  All operation runtime below is owned by this one task.
 * Scheduler/BLE/timer callbacks only copy an event into ds_events. */
#include <string.h>

#include "device_control_scheduler.h"
#include "device_settings_internal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "gw_settings_view.h"

static const char *TAG = "ds_actor";
static const uint32_t s_retry_backoff_ms[] = { 100, 250, 500 };
#define DS_WORK_RETRY_MAX (sizeof(s_retry_backoff_ms) / sizeof(s_retry_backoff_ms[0]))

typedef enum { DS_ACTOR_IDLE, DS_ACTOR_WAIT_LEASE, DS_ACTOR_WAIT_COMMAND,
               DS_ACTOR_WAIT_READ_STREAM, DS_ACTOR_WAIT_RETRY } ds_actor_phase_t;
typedef struct {
    int16_t slot;
    uint32_t generation;
    device_control_owner_token_t owner;
} ds_cookie_t;
typedef struct {
    bool active;
    ds_actor_phase_t phase;
    int16_t slot;
    uint32_t generation;
    device_control_owner_token_t owner;
    device_control_lease_t lease;
    uint8_t retry_count;
    bool read_ack;
    bool values_end;
    bool values_ok;
    ds_cookie_t cookie;
} ds_active_op_t;
typedef struct {
    TaskHandle_t task;
    volatile bool running;
    bool scheduler_owned;
    TimerHandle_t retry_timer;
} ds_worker_t;

static ds_active_op_t s_active_op;
static ds_worker_t s_worker;
static ds_cookie_t s_tx_cookie;
static char s_tx_device[GW_MSG_DEVICE_ID_LEN];

extern bool s_ops_active[];
extern uint32_t s_ops_generation[];
extern void s_ops_invoke_completion(int slot, ds_op_result_t result);
extern const char *s_ops_get_device_id(int slot);
extern ds_op_kind_t s_ops_get_kind(int slot);
extern ds_op_state_t s_ops_get_state(int slot);
extern void s_ops_set_state(int slot, ds_op_state_t state);
extern void s_ops_set_kind(int slot, ds_op_kind_t kind);

static void actor_task(void *arg);

static void release_lease(void)
{
    if (s_active_op.lease == 0) return;
    esp_err_t err = device_control_scheduler_release_lease(s_active_op.lease);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "lease %lu release: %s", (unsigned long)s_active_op.lease,
                 esp_err_to_name(err));
    }
    s_active_op.lease = 0;
}

static void finish_active(ds_op_result_t result)
{
    if (!s_active_op.active) return;
    int slot = s_active_op.slot;
    const char *id = s_ops_get_device_id(slot);
    (void)device_control_scheduler_cancel_source(id, DEVICE_CTRL_SOURCE_SETTINGS,
                                                  s_active_op.owner);
    release_lease();
    memset(&s_active_op, 0, sizeof(s_active_op));
    if (slot >= 0 && slot < DEVICE_SETTINGS_MAX_DEVICES) {
        s_ops_invoke_completion(slot, result);
    }
}

static bool matching(const ds_event_t *ev)
{
    return s_active_op.active && ev->slot == s_active_op.slot &&
           ev->generation == s_active_op.generation &&
           ev->owner_token == s_active_op.owner;
}

static void post_command_done(const device_control_result_t *result, void *context)
{
    const ds_cookie_t *cookie = context;
    if (cookie == NULL) return;
    ds_event_t ev = { .type = DS_EVENT_COMMAND_COMPLETE, .slot = cookie->slot,
                      .generation = cookie->generation, .owner_token = cookie->owner };
    if (result == NULL) ev.data.command.status = DEVICE_CMD_STATUS_INTERNAL;
    else if (result->terminal == DEVICE_CTRL_TERMINAL_COMMAND) ev.data.command = result->command_result;
    else ev.data.command.status = DEVICE_CMD_STATUS_CANCELLED;
    (void)ds_events_post(&ev);
}

static void post_lease_done(esp_err_t status, device_control_lease_t lease, void *context)
{
    const ds_cookie_t *cookie = context;
    if (cookie == NULL) return;
    ds_event_t ev = { .type = DS_EVENT_LEASE_RESULT, .slot = cookie->slot,
                      .generation = cookie->generation, .owner_token = cookie->owner };
    ev.data.lease.status = status;
    ev.data.lease.id = lease;
    (void)ds_events_post(&ev);
}

static void post_tx_lease_done(esp_err_t status, device_control_lease_t lease, void *context)
{
    const ds_cookie_t *cookie = context;
    if (cookie == NULL) return;
    ds_event_t ev = { .type = DS_EVENT_LEASE_RESULT, .slot = -1,
        .generation = cookie->generation, .owner_token = cookie->owner };
    ev.data.lease.status = status; ev.data.lease.id = lease;
    (void)ds_events_post(&ev);
}

static esp_err_t submit_command(void)
{
    device_command_request_t req = { .origin = DEVICE_CMD_ORIGIN_SETTINGS };
    const char *id = s_ops_get_device_id(s_active_op.slot);
    strlcpy(req.device_id, id, sizeof(req.device_id));
    if (s_ops_get_kind(s_active_op.slot) == DS_OP_DESCRIBE)
        strlcpy(req.command, GW_SETTINGS_CMD_DESCRIBE_SETTINGS, sizeof(req.command));
    else if (s_ops_get_kind(s_active_op.slot) == DS_OP_GET)
        strlcpy(req.command, GW_SETTINGS_CMD_READ_SETTINGS, sizeof(req.command));
    else return ESP_ERR_INVALID_ARG;
    device_control_job_t job = { .request = req, .priority = DEVICE_CTRL_PRIORITY_NORMAL,
        .source = DEVICE_CTRL_SOURCE_SETTINGS, .owner_token = s_active_op.owner,
        .lease_id = s_active_op.lease, .max_queue_wait_ms = 10000 };
    return device_control_scheduler_submit(&job, post_command_done, &s_active_op.cookie, NULL);
}

static void schedule_retry(void)
{
    if (s_active_op.retry_count >= DS_WORK_RETRY_MAX || s_worker.retry_timer == NULL) {
        finish_active(DS_OP_RESULT_FAILED); return;
    }
    uint32_t delay = s_retry_backoff_ms[s_active_op.retry_count++];
    s_active_op.phase = DS_ACTOR_WAIT_RETRY;
    (void)xTimerChangePeriod(s_worker.retry_timer, pdMS_TO_TICKS(delay), 0);
    (void)xTimerStart(s_worker.retry_timer, 0);
}

static void retry_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    ds_cookie_t cookie = s_active_op.cookie; /* immutable cookie while timer is armed */
    ds_event_t ev = { .type = DS_EVENT_RETRY_TIMER, .slot = cookie.slot,
                      .generation = cookie.generation, .owner_token = cookie.owner };
    (void)ds_events_post(&ev);
}

static void handle_start(void)
{
    if (s_active_op.active) return;
    for (int slot = 0; slot < DEVICE_SETTINGS_MAX_DEVICES; slot++) {
        if (!s_ops_active[slot] || s_ops_get_state(slot) != DS_OP_QUEUED) continue;
        memset(&s_active_op, 0, sizeof(s_active_op));
        s_active_op.active = true; s_active_op.slot = slot;
        s_active_op.generation = s_ops_generation[slot];
        s_active_op.owner = s_active_op.generation ? s_active_op.generation : 1;
        s_active_op.cookie = (ds_cookie_t){ slot, s_active_op.generation, s_active_op.owner };
        s_active_op.phase = DS_ACTOR_WAIT_LEASE;
        device_control_lease_request_t request = { .device_id = s_ops_get_device_id(slot),
            .priority = DEVICE_CTRL_PRIORITY_NORMAL, .source = DEVICE_CTRL_SOURCE_SETTINGS,
            .owner_token = s_active_op.owner };
        if (device_control_scheduler_acquire_lease(&request, post_lease_done,
                                                   &s_active_op.cookie) != ESP_OK) {
            finish_active(DS_OP_RESULT_INTERNAL);
        }
        return;
    }
}

static void handle_lease(const ds_event_t *ev)
{
    if (ev->slot == -1 && ev->generation == s_tx_cookie.generation &&
        ev->owner_token == s_tx_cookie.owner) {
        if (ev->data.lease.status == ESP_OK && ev->data.lease.id != 0)
            device_settings_tx_actor_begin(s_tx_device, s_tx_cookie.owner, ev->data.lease.id);
        else
            device_settings_tx_actor_abort(s_tx_device);
        return;
    }
    if (!matching(ev)) {
        if (ev->data.lease.id != 0) (void)device_control_scheduler_release_lease(ev->data.lease.id);
        return;
    }
    if (ev->data.lease.status != ESP_OK || ev->data.lease.id == 0) { finish_active(DS_OP_RESULT_INTERNAL); return; }
    s_active_op.lease = ev->data.lease.id;
    if (submit_command() != ESP_OK) schedule_retry();
    else { s_active_op.phase = DS_ACTOR_WAIT_COMMAND; s_ops_set_state(s_active_op.slot, DS_OP_RUNNING); }
}

static void complete_read_if_ready(void)
{
    if (s_active_op.active && s_active_op.read_ack && s_active_op.values_end)
        finish_active(s_active_op.values_ok ? DS_OP_RESULT_OK : DS_OP_RESULT_PROTOCOL_ERROR);
}

static void handle_command(const ds_event_t *ev)
{
    if (!matching(ev)) return;
    switch (ev->data.command.status) {
    case DEVICE_CMD_STATUS_OK:
        if (s_ops_get_kind(s_active_op.slot) == DS_OP_DESCRIBE) {
            s_ops_set_kind(s_active_op.slot, DS_OP_GET);
            if (submit_command() != ESP_OK) schedule_retry();
            else s_active_op.phase = DS_ACTOR_WAIT_COMMAND;
        } else { s_active_op.read_ack = true; s_active_op.phase = DS_ACTOR_WAIT_READ_STREAM; complete_read_if_ready(); }
        break;
    case DEVICE_CMD_STATUS_BUSY: schedule_retry(); break;
    case DEVICE_CMD_STATUS_TIMEOUT: finish_active(DS_OP_RESULT_TIMEOUT); break;
    case DEVICE_CMD_STATUS_REJECTED: finish_active(DS_OP_RESULT_REJECTED); break;
    default: finish_active(DS_OP_RESULT_INTERNAL); break;
    }
}

static void handle_values(const ds_event_t *ev)
{
    if (!s_active_op.active || s_ops_get_kind(s_active_op.slot) != DS_OP_GET ||
        strncmp(s_ops_get_device_id(s_active_op.slot), ev->device_id, GW_MSG_DEVICE_ID_LEN) != 0) return;
    s_active_op.values_end = true; s_active_op.values_ok = ev->data.values.success;
    complete_read_if_ready();
}

static void cancel_device(const char *id, bool forget)
{
    if (s_active_op.active && strncmp(s_ops_get_device_id(s_active_op.slot), id, GW_MSG_DEVICE_ID_LEN) == 0) {
        s_ops_generation[s_active_op.slot]++; /* invalidate before terminal cleanup */
        finish_active(DS_OP_RESULT_INTERNAL);
    }
    (void)forget;
    handle_start();
}

static void actor_task(void *arg)
{
    (void)arg; ds_event_t ev;
    while (s_worker.running && ds_events_wait(&ev)) {
        switch (ev.type) {
        case DS_EVENT_START_OPERATION: handle_start(); break;
        case DS_EVENT_LEASE_RESULT: handle_lease(&ev); break;
        case DS_EVENT_COMMAND_COMPLETE:
            if (ev.slot == -1) device_settings_tx_actor_command_complete(ev.device_id, &ev.data.command);
            else handle_command(&ev);
            break;
        case DS_EVENT_VALUES_STREAM_COMPLETE: handle_values(&ev); break;
        case DS_EVENT_RETRY_TIMER: if (matching(&ev) && submit_command() == ESP_OK) s_active_op.phase = DS_ACTOR_WAIT_COMMAND; else if (matching(&ev)) schedule_retry(); break;
        case DS_EVENT_TRANSACTION_START: {
            s_tx_cookie = (ds_cookie_t){ -1, ev.generation, ev.owner_token };
            strlcpy(s_tx_device, ev.device_id, sizeof(s_tx_device));
            device_control_lease_request_t request = { .device_id = s_tx_device,
                .priority = DEVICE_CTRL_PRIORITY_HIGH, .source = DEVICE_CTRL_SOURCE_SETTINGS,
                .owner_token = s_tx_cookie.owner };
            if (device_control_scheduler_acquire_lease(&request, post_tx_lease_done,
                                                       &s_tx_cookie) != ESP_OK)
                device_settings_tx_actor_abort(s_tx_device);
            break;
        }
        case DS_EVENT_TRANSACTION_ABORT: device_settings_tx_actor_abort(ev.device_id); break;
        case DS_EVENT_TRANSACTION_TIMEOUT: device_settings_tx_actor_timeout(ev.device_id); break;
        case DS_EVENT_DISCONNECTED:
            cancel_device(ev.device_id, false);
            if (!device_settings_tx_actor_on_disconnect(ev.device_id))
                device_settings_tx_actor_abort(ev.device_id);
            break;
        case DS_EVENT_CANCEL_DEVICE: case DS_EVENT_FORGET_DEVICE:
            cancel_device(ev.device_id, ev.type == DS_EVENT_FORGET_DEVICE);
            device_settings_tx_actor_abort(ev.device_id);
            break;
        case DS_EVENT_SHUTDOWN: s_worker.running = false; break;
        default: break; /* Transaction events are owned by its actor migration. */
        }
    }
    s_worker.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t device_settings_worker_init(void)
{
    if (s_worker.task != NULL) return ESP_ERR_INVALID_STATE;
    memset(&s_active_op, 0, sizeof(s_active_op));
    esp_err_t scheduler_err = device_control_scheduler_init();
    if (scheduler_err == ESP_OK) s_worker.scheduler_owned = true;
    else if (scheduler_err != ESP_ERR_INVALID_STATE) return scheduler_err;
    if (ds_events_init() != ESP_OK) {
        if (s_worker.scheduler_owned) device_control_scheduler_deinit();
        s_worker.scheduler_owned = false;
        return ESP_ERR_NO_MEM;
    }
    s_worker.retry_timer = xTimerCreate("ds_retry", pdMS_TO_TICKS(100), pdFALSE, NULL, retry_timer_cb);
    if (s_worker.retry_timer == NULL) {
        ds_events_deinit();
        if (s_worker.scheduler_owned) device_control_scheduler_deinit();
        s_worker.scheduler_owned = false;
        return ESP_ERR_NO_MEM;
    }
    s_worker.running = true;
    if (xTaskCreate(actor_task, "ds_actor", 4096, NULL, 4, &s_worker.task) != pdPASS) {
        s_worker.running = false; xTimerDelete(s_worker.retry_timer, 0); s_worker.retry_timer = NULL;
        ds_events_deinit(); if (s_worker.scheduler_owned) device_control_scheduler_deinit();
        s_worker.scheduler_owned = false; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void device_settings_worker_deinit(void)
{
    if (s_worker.task == NULL) return;
    ds_event_t ev = { .type = DS_EVENT_SHUTDOWN }; (void)ds_events_post(&ev);
    for (int i=0; i<50 && s_worker.task != NULL; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_worker.retry_timer != NULL) { xTimerStop(s_worker.retry_timer, 0); xTimerDelete(s_worker.retry_timer, 0); s_worker.retry_timer = NULL; }
    ds_events_deinit();
    if (s_worker.scheduler_owned) device_control_scheduler_deinit();
    s_worker.scheduler_owned = false;
    memset(&s_active_op, 0, sizeof(s_active_op));
}
void device_settings_worker_submit(void) { ds_event_t ev = { .type = DS_EVENT_START_OPERATION }; (void)ds_events_post(&ev); }
void device_settings_worker_on_disconnect(const char *id) { if (id) { ds_event_t ev = { .type = DS_EVENT_DISCONNECTED }; strlcpy(ev.device_id,id,sizeof(ev.device_id)); (void)ds_events_post(&ev); } }
void device_settings_worker_on_values_complete(const char *id, bool success) { if (id) { ds_event_t ev = { .type = DS_EVENT_VALUES_STREAM_COMPLETE }; strlcpy(ev.device_id,id,sizeof(ev.device_id)); ev.data.values.success=success; (void)ds_events_post(&ev); } }
void device_settings_worker_reset_for_test(void) { memset(&s_active_op, 0, sizeof(s_active_op)); }
bool device_settings_worker_is_idle_for_test(void) { return !s_active_op.active; }
uint32_t device_settings_worker_get_generation_for_test(void) { return s_active_op.generation; }
bool device_settings_worker_actor_running(void) { return s_worker.running && s_worker.task != NULL; }
