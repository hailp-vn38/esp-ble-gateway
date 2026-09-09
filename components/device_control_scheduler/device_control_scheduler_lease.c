#include "device_control_scheduler_internal.h"
#include <string.h>
#include "esp_timer.h"

esp_err_t device_control_scheduler_acquire_lease(const device_control_lease_request_t *request, device_control_lease_completion_fn completion, void *context)
{
    if(!request||!completion||!request->device_id||!request->device_id[0]||request->priority>DEVICE_CTRL_PRIORITY_BACKGROUND)return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&g_ctrl.mux); if(!g_ctrl.running){taskEXIT_CRITICAL(&g_ctrl.mux);return ESP_ERR_INVALID_STATE;} int d=ctrl_find_device_locked(request->device_id,true);int slot=-1;for(int i=0;i<DEVICE_CTRL_MAX_DEVICES;i++)if(g_ctrl.leases[i].state==CTRL_LEASE_FREE){slot=i;break;}if(d<0||slot<0){g_ctrl.stats.queue_full++;taskEXIT_CRITICAL(&g_ctrl.mux);return ESP_ERR_NO_MEM;}
    ctrl_lease_t *l=&g_ctrl.leases[slot];memset(l,0,sizeof(*l));l->state=CTRL_LEASE_PENDING;l->id=++g_ctrl.next_lease;if(!l->id)l->id=++g_ctrl.next_lease;l->device_index=d;l->priority=request->priority;l->source=request->source;l->owner=request->owner_token;l->completion=completion;l->context=context;taskEXIT_CRITICAL(&g_ctrl.mux);ctrl_wake();return ESP_OK;
}
esp_err_t device_control_scheduler_release_lease(device_control_lease_t id)
{ if(!id)return ESP_ERR_INVALID_ARG; taskENTER_CRITICAL(&g_ctrl.mux);for(int i=0;i<DEVICE_CTRL_MAX_DEVICES;i++){ctrl_lease_t*l=&g_ctrl.leases[i];if(l->state==CTRL_LEASE_ACTIVE&&l->id==id){ctrl_device_t*d=&g_ctrl.devices[l->device_index];if(d->lease!=id){taskEXIT_CRITICAL(&g_ctrl.mux);return ESP_ERR_INVALID_STATE;}uint32_t held=(esp_timer_get_time()-d->lease_started_us)/1000;if(held>g_ctrl.stats.lease_hold_max_ms)g_ctrl.stats.lease_hold_max_ms=held;d->lease=0;d->lease_owner=0;l->state=CTRL_LEASE_FREE;taskEXIT_CRITICAL(&g_ctrl.mux);ctrl_wake();return ESP_OK;}}taskEXIT_CRITICAL(&g_ctrl.mux);return ESP_ERR_NOT_FOUND; }
void ctrl_process_leases(void)
{
    for (int i = 0; i < DEVICE_CTRL_MAX_DEVICES; i++) {
        device_control_lease_completion_fn cb = NULL;
        void *ctx = NULL;
        device_control_lease_t id = 0;
        esp_err_t status = ESP_OK;
        taskENTER_CRITICAL(&g_ctrl.mux);
        ctrl_lease_t *l = &g_ctrl.leases[i];
        if (l->state == CTRL_LEASE_COMPLETION_PENDING) {
            cb = l->completion; ctx = l->context; id = l->id; status = l->completion_status;
            memset(l, 0, sizeof(*l));
        } else if (l->state == CTRL_LEASE_PENDING) {
            ctrl_device_t *d = &g_ctrl.devices[l->device_index];
            if (!d->blocked && !d->transport_inflight && !d->lease) {
                d->lease = l->id; d->lease_owner = l->owner; d->lease_source = l->source;
                d->lease_started_us = esp_timer_get_time(); l->state = CTRL_LEASE_ACTIVE;
                g_ctrl.stats.lease_granted++; ctrl_stats_locked();
                cb = l->completion; ctx = l->context; id = l->id;
            }
        }
        taskEXIT_CRITICAL(&g_ctrl.mux);
        if (cb) cb(status, id, ctx);
    }
}
