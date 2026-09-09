#include "device_control_scheduler_internal.h"
#include <string.h>
#include "esp_timer.h"

static void make_terminal(ctrl_slot_t *s, device_control_terminal_t terminal)
{ s->terminal = terminal; memset(&s->pending_result, 0, sizeof(s->pending_result)); s->state = CTRL_SLOT_COMPLETION_PENDING; }

void ctrl_dcs_complete(const device_command_result_t *result, void *context)
{
    ctrl_dcs_context_t *ctx = context;
    if (!ctx || ctx->slot >= DEVICE_CTRL_MAX_JOBS) return;
    taskENTER_CRITICAL(&g_ctrl.mux);
    ctrl_slot_t *s = &g_ctrl.slots[ctx->slot];
    if (s->state != CTRL_SLOT_INFLIGHT || s->generation != ctx->generation || s->completion_written) {
        g_ctrl.stats.completion_mailbox_conflict++; taskEXIT_CRITICAL(&g_ctrl.mux); return;
    }
    s->pending_result = result ? *result : (device_command_result_t){ .status = DEVICE_CMD_STATUS_INTERNAL };
    s->terminal = DEVICE_CTRL_TERMINAL_COMMAND; s->completion_written = true; s->state = CTRL_SLOT_COMPLETION_PENDING;
    taskEXIT_CRITICAL(&g_ctrl.mux); ctrl_wake();
}

void ctrl_complete_slot(uint16_t index)
{
    device_control_completion_fn completion; void *context; device_control_result_t result;
    taskENTER_CRITICAL(&g_ctrl.mux);
    ctrl_slot_t *s = &g_ctrl.slots[index];
    if (s->state != CTRL_SLOT_COMPLETION_PENDING) { taskEXIT_CRITICAL(&g_ctrl.mux); return; }
    ctrl_device_t *d = &g_ctrl.devices[s->device_index];
    if (d->inflight_slot == (int16_t)index) { d->inflight_slot = -1; d->transport_inflight = false; if (g_ctrl.inflight) g_ctrl.inflight--; }
    completion = s->completion; context = s->context; result.scheduler_job_id = s->id; result.terminal = s->terminal; result.command_result = s->pending_result;
    if (result.terminal == DEVICE_CTRL_TERMINAL_SUPERSEDED) g_ctrl.stats.superseded++;
    else if (result.terminal == DEVICE_CTRL_TERMINAL_DEADLINE_EXCEEDED) g_ctrl.stats.deadline_exceeded++;
    else if (result.terminal == DEVICE_CTRL_TERMINAL_COMMAND) g_ctrl.stats.completed++;
    s->state = CTRL_SLOT_COMPLETING; taskEXIT_CRITICAL(&g_ctrl.mux);
    completion(&result, context);
    taskENTER_CRITICAL(&g_ctrl.mux);
    uint32_t generation = s->generation;
    memset(s, 0, sizeof(*s));
    /* Keep an ABA guard across pool-slot reuse. */
    s->generation = generation;
    taskEXIT_CRITICAL(&g_ctrl.mux);
}

static int select_slot_locked(void)
{
    int best = -1; device_control_priority_t best_prio = DEVICE_CTRL_PRIORITY_BACKGROUND;
    bool normal_available = false;
    for (int i=0;i<DEVICE_CTRL_MAX_JOBS;i++) if (g_ctrl.slots[i].state == CTRL_SLOT_QUEUED && g_ctrl.slots[i].job.priority == DEVICE_CTRL_PRIORITY_NORMAL) normal_available=true;
    for (int offset=0;offset<DEVICE_CTRL_MAX_DEVICES;offset++) { int d=(g_ctrl.rr_device+offset)%DEVICE_CTRL_MAX_DEVICES; ctrl_device_t *dev=&g_ctrl.devices[d];
        if(!dev->used||dev->blocked||dev->transport_inflight) continue;
        for(int i=0;i<DEVICE_CTRL_MAX_JOBS;i++) { ctrl_slot_t *s=&g_ctrl.slots[i]; if(s->state!=CTRL_SLOT_QUEUED||s->device_index!=d)continue;
            if(dev->lease && s->job.lease_id != dev->lease) continue;
            if (g_ctrl.high_burst >= DEVICE_CTRL_HIGH_BURST_MAX && normal_available && s->job.priority == DEVICE_CTRL_PRIORITY_HIGH) continue;
            if(best<0 || s->job.priority<best_prio){best=i;best_prio=s->job.priority;}
        }
    }
    return best;
}

static void dispatch_one(void)
{
    uint16_t index; device_command_request_t request; ctrl_dcs_context_t *context;
    taskENTER_CRITICAL(&g_ctrl.mux);
    if (g_ctrl.inflight >= DEVICE_CTRL_MAX_INFLIGHT) { taskEXIT_CRITICAL(&g_ctrl.mux); return; }
    int selected=select_slot_locked(); if(selected<0){taskEXIT_CRITICAL(&g_ctrl.mux);return;} index=selected; ctrl_slot_t *s=&g_ctrl.slots[index]; ctrl_device_t *d=&g_ctrl.devices[s->device_index];
    s->state=CTRL_SLOT_INFLIGHT; d->transport_inflight=true; d->inflight_slot=index; g_ctrl.inflight++; g_ctrl.rr_device=(s->device_index+1)%DEVICE_CTRL_MAX_DEVICES;
    if(s->job.priority==DEVICE_CTRL_PRIORITY_HIGH)g_ctrl.high_burst++;else if(s->job.priority==DEVICE_CTRL_PRIORITY_NORMAL)g_ctrl.high_burst=0;
    s->dcs_context.slot=index; s->dcs_context.generation=s->generation;
    context=&s->dcs_context; request=s->job.request;
    g_ctrl.stats.dispatched++; ctrl_stats_locked(); taskEXIT_CRITICAL(&g_ctrl.mux);
    esp_err_t err=device_command_service_submit(&request,ctrl_dcs_complete,context);
    if(err!=ESP_OK){ taskENTER_CRITICAL(&g_ctrl.mux); s=&g_ctrl.slots[index]; if(s->state==CTRL_SLOT_INFLIGHT){s->pending_result.status=err==ESP_ERR_NO_MEM?DEVICE_CMD_STATUS_QUEUE_FULL:DEVICE_CMD_STATUS_INTERNAL;s->terminal=DEVICE_CTRL_TERMINAL_COMMAND;s->completion_written=true;s->state=CTRL_SLOT_COMPLETION_PENDING;}taskEXIT_CRITICAL(&g_ctrl.mux);ctrl_wake(); }
}

void ctrl_process(void)
{
    int64_t now=esp_timer_get_time();
    taskENTER_CRITICAL(&g_ctrl.mux);
    for(uint16_t i=0;i<DEVICE_CTRL_MAX_JOBS;i++){ctrl_slot_t *s=&g_ctrl.slots[i];if(s->state==CTRL_SLOT_ENQUEUE_PENDING)s->state=CTRL_SLOT_QUEUED; if(s->state==CTRL_SLOT_QUEUED&&s->deadline_us&&now>=s->deadline_us)make_terminal(s,DEVICE_CTRL_TERMINAL_DEADLINE_EXCEEDED);}
    taskEXIT_CRITICAL(&g_ctrl.mux);
    ctrl_process_leases();
    for(uint16_t i=0;i<DEVICE_CTRL_MAX_JOBS;i++)ctrl_complete_slot(i);
    for(int n=0;n<DEVICE_CTRL_MAX_INFLIGHT;n++)dispatch_one();
}

void ctrl_worker(void *arg)
{ (void)arg; while(g_ctrl.running){ctrl_process();(void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(20));} ctrl_process();g_ctrl.stopped=true;vTaskDelete(NULL); }
