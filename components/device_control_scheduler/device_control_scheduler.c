#include "device_control_scheduler_internal.h"
#include <string.h>
#include "esp_timer.h"

ctrl_state_t g_ctrl = { .mux = portMUX_INITIALIZER_UNLOCKED };

void ctrl_wake(void) { if (g_ctrl.task != NULL) xTaskNotifyGive(g_ctrl.task); }

int16_t ctrl_find_device_locked(const char *id, bool create)
{
    for (int i = 0; i < DEVICE_CTRL_MAX_DEVICES; i++)
        if (g_ctrl.devices[i].used && strcmp(g_ctrl.devices[i].device_id, id) == 0) return i;
    if (!create) return -1;
    for (int i = 0; i < DEVICE_CTRL_MAX_DEVICES; i++) if (!g_ctrl.devices[i].used) {
        g_ctrl.devices[i].used = true; g_ctrl.devices[i].inflight_slot = -1;
        strlcpy(g_ctrl.devices[i].device_id, id, sizeof(g_ctrl.devices[i].device_id)); return i;
    }
    return -1;
}

bool ctrl_job_equal(const device_control_job_t *a, const device_control_job_t *b)
{
    return a->dedupe_hash == b->dedupe_hash && a->source == b->source &&
           a->owner_token == b->owner_token && a->request.origin == b->request.origin &&
           memcmp(&a->request, &b->request, sizeof(a->request)) == 0;
}

void ctrl_stats_locked(void)
{
    uint32_t queued = 0, leases = 0;
    for (int i = 0; i < DEVICE_CTRL_MAX_JOBS; i++)
        if (g_ctrl.slots[i].state == CTRL_SLOT_ENQUEUE_PENDING || g_ctrl.slots[i].state == CTRL_SLOT_QUEUED) queued++;
    for (int i = 0; i < DEVICE_CTRL_MAX_DEVICES; i++) if (g_ctrl.devices[i].lease != 0) leases++;
    if (queued > g_ctrl.stats.max_queued) g_ctrl.stats.max_queued = queued;
    if (g_ctrl.inflight > g_ctrl.stats.max_inflight) g_ctrl.stats.max_inflight = g_ctrl.inflight;
    if (leases > g_ctrl.stats.max_active_leases) g_ctrl.stats.max_active_leases = leases;
}

esp_err_t device_control_scheduler_init(void)
{
    taskENTER_CRITICAL(&g_ctrl.mux);
    if (g_ctrl.task != NULL) { taskEXIT_CRITICAL(&g_ctrl.mux); return ESP_ERR_INVALID_STATE; }
    memset(g_ctrl.slots, 0, sizeof(g_ctrl.slots)); memset(g_ctrl.devices, 0, sizeof(g_ctrl.devices));
    memset(g_ctrl.leases, 0, sizeof(g_ctrl.leases)); memset(&g_ctrl.stats, 0, sizeof(g_ctrl.stats));
    g_ctrl.next_job = g_ctrl.next_lease = 0; g_ctrl.inflight = g_ctrl.rr_device = g_ctrl.high_burst = 0;
    g_ctrl.running = true; g_ctrl.stopped = false;
    taskEXIT_CRITICAL(&g_ctrl.mux);
    if (xTaskCreate(ctrl_worker, "dev_ctrl", DEVICE_CTRL_TASK_STACK, NULL,
                    DEVICE_CTRL_TASK_PRIORITY, &g_ctrl.task) != pdPASS) {
        g_ctrl.running = false; g_ctrl.task = NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void device_control_scheduler_deinit(void)
{
    if (g_ctrl.task == NULL) return;
    taskENTER_CRITICAL(&g_ctrl.mux);
    for (uint16_t i = 0; i < DEVICE_CTRL_MAX_JOBS; i++) if (g_ctrl.slots[i].state != CTRL_SLOT_FREE && g_ctrl.slots[i].state != CTRL_SLOT_INFLIGHT) {
        g_ctrl.slots[i].terminal = DEVICE_CTRL_TERMINAL_CANCELLED; g_ctrl.slots[i].state = CTRL_SLOT_COMPLETION_PENDING;
    }
    taskEXIT_CRITICAL(&g_ctrl.mux); ctrl_wake(); vTaskDelay(pdMS_TO_TICKS(20));
    g_ctrl.running = false; ctrl_wake();
    for (int ms = 0; ms < 1000 && !g_ctrl.stopped; ms += 10) vTaskDelay(pdMS_TO_TICKS(10));
    g_ctrl.task = NULL;
}

esp_err_t device_control_scheduler_submit(const device_control_job_t *job, device_control_completion_fn completion, void *context, device_control_job_id_t *out)
{
    if (!job || !completion || !job->request.device_id[0] || job->priority > DEVICE_CTRL_PRIORITY_BACKGROUND || job->source > DEVICE_CTRL_SOURCE_INTERNAL) return ESP_ERR_INVALID_ARG;
    if (job->dedupe != DEVICE_CTRL_DEDUPE_NONE && job->priority != DEVICE_CTRL_PRIORITY_BACKGROUND) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&g_ctrl.mux);
    if (!g_ctrl.running) { taskEXIT_CRITICAL(&g_ctrl.mux); return ESP_ERR_INVALID_STATE; }
    int16_t device = ctrl_find_device_locked(job->request.device_id, true);
    if (device < 0) { g_ctrl.stats.queue_full++; taskEXIT_CRITICAL(&g_ctrl.mux); return ESP_ERR_NO_MEM; }
    for (uint16_t i = 0; i < DEVICE_CTRL_MAX_JOBS; i++) if (g_ctrl.slots[i].state == CTRL_SLOT_QUEUED && job->dedupe != DEVICE_CTRL_DEDUPE_NONE && ctrl_job_equal(&g_ctrl.slots[i].job, job)) {
        g_ctrl.stats.deduped++;
        if (job->dedupe == DEVICE_CTRL_DEDUPE_DROP_IF_EXISTS) { if (out) *out = g_ctrl.slots[i].id; taskEXIT_CRITICAL(&g_ctrl.mux); return ESP_ERR_INVALID_STATE; }
        g_ctrl.slots[i].terminal = DEVICE_CTRL_TERMINAL_SUPERSEDED; g_ctrl.slots[i].state = CTRL_SLOT_COMPLETION_PENDING;
    }
    int slot = -1; for (int i = 0; i < DEVICE_CTRL_MAX_JOBS; i++) if (g_ctrl.slots[i].state == CTRL_SLOT_FREE) { slot = i; break; }
    if (slot < 0) { g_ctrl.stats.queue_full++; taskEXIT_CRITICAL(&g_ctrl.mux); return ESP_ERR_NO_MEM; }
    ctrl_slot_t *s = &g_ctrl.slots[slot]; uint32_t generation = s->generation + 1;
    memset(s, 0, sizeof(*s)); s->state = CTRL_SLOT_ENQUEUE_PENDING;
    s->generation = generation ? generation : 1; s->id = ++g_ctrl.next_job; if (!s->id) s->id = ++g_ctrl.next_job;
    s->job = *job; s->completion = completion; s->context = context; s->device_index = device; s->enqueue_us = esp_timer_get_time();
    s->deadline_us = job->max_queue_wait_ms ? s->enqueue_us + (int64_t)job->max_queue_wait_ms * 1000 : 0;
    g_ctrl.stats.submitted++; ctrl_stats_locked(); if (out) *out = s->id; taskEXIT_CRITICAL(&g_ctrl.mux); ctrl_wake(); return ESP_OK;
}

static esp_err_t ctrl_cancel_match(const char *device_id, device_control_source_t source, device_control_owner_token_t owner, bool all_sources, device_control_job_id_t id)
{
    bool found = false, cancel_inflight = false; char inflight_device[GW_MSG_DEVICE_ID_LEN] = {0};
    taskENTER_CRITICAL(&g_ctrl.mux);
    for (uint16_t i = 0; i < DEVICE_CTRL_MAX_JOBS; i++) { ctrl_slot_t *s = &g_ctrl.slots[i];
        if (s->state == CTRL_SLOT_FREE || (id && s->id != id) || (!id && strcmp(s->job.request.device_id, device_id) != 0) || (!all_sources && (s->job.source != source || s->job.owner_token != owner))) continue;
        found = true;
        if (s->state == CTRL_SLOT_INFLIGHT) { cancel_inflight = true; strlcpy(inflight_device, s->job.request.device_id, sizeof(inflight_device)); }
        else { s->terminal = DEVICE_CTRL_TERMINAL_CANCELLED; s->state = CTRL_SLOT_COMPLETION_PENDING; g_ctrl.stats.cancelled++; }
    }
    if (!id) for (uint16_t i = 0; i < DEVICE_CTRL_MAX_DEVICES; i++) {
        ctrl_lease_t *lease = &g_ctrl.leases[i];
        if (lease->state == CTRL_LEASE_FREE || strcmp(g_ctrl.devices[lease->device_index].device_id, device_id) != 0 ||
            (!all_sources && (lease->source != source || lease->owner != owner))) continue;
        if (lease->state == CTRL_LEASE_ACTIVE) {
            g_ctrl.devices[lease->device_index].lease = 0;
            g_ctrl.stats.lease_revoked++;
            lease->state = CTRL_LEASE_FREE;
        } else {
            /* Accepted async lease requests own one callback, even when cancelled. */
            lease->completion_status = ESP_ERR_INVALID_STATE;
            lease->state = CTRL_LEASE_COMPLETION_PENDING;
        }
        found = true;
    }
    taskEXIT_CRITICAL(&g_ctrl.mux); if (cancel_inflight) device_command_service_cancel_device(inflight_device); if (found) ctrl_wake(); return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
esp_err_t device_control_scheduler_cancel_job(device_control_job_id_t id) { return id ? ctrl_cancel_match(NULL, 0, 0, true, id) : ESP_ERR_INVALID_ARG; }
esp_err_t device_control_scheduler_cancel_source(const char *id, device_control_source_t s, device_control_owner_token_t o) { return id ? ctrl_cancel_match(id, s, o, false, 0) : ESP_ERR_INVALID_ARG; }
esp_err_t device_control_scheduler_cancel_device(const char *id) { return id ? ctrl_cancel_match(id, 0, 0, true, 0) : ESP_ERR_INVALID_ARG; }

esp_err_t device_control_scheduler_block_device(const char *id) { if (!id || !id[0]) return ESP_ERR_INVALID_ARG; taskENTER_CRITICAL(&g_ctrl.mux); int d = ctrl_find_device_locked(id, true); if (d >= 0) g_ctrl.devices[d].blocked = true; taskEXIT_CRITICAL(&g_ctrl.mux); return d >= 0 ? ESP_OK : ESP_ERR_NO_MEM; }
esp_err_t device_control_scheduler_unblock_device(const char *id) { if (!id) return ESP_ERR_INVALID_ARG; taskENTER_CRITICAL(&g_ctrl.mux); int d = ctrl_find_device_locked(id, false); if (d >= 0) g_ctrl.devices[d].blocked = false; taskEXIT_CRITICAL(&g_ctrl.mux); ctrl_wake(); return d >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND; }
bool device_control_scheduler_is_idle(const char *id) { bool idle = true; taskENTER_CRITICAL(&g_ctrl.mux); int d = ctrl_find_device_locked(id, false); if (d >= 0) { idle = !g_ctrl.devices[d].transport_inflight && !g_ctrl.devices[d].lease; for (int i=0;i<DEVICE_CTRL_MAX_JOBS;i++) if (g_ctrl.slots[i].state != CTRL_SLOT_FREE && g_ctrl.slots[i].device_index == d) idle=false; } taskEXIT_CRITICAL(&g_ctrl.mux); return idle; }
esp_err_t device_control_scheduler_quiesce_device(const char *id, uint32_t timeout_ms) { esp_err_t e=device_control_scheduler_block_device(id); if(e!=ESP_OK)return e; device_control_scheduler_cancel_device(id); for(uint32_t t=0;t<timeout_ms;t+=10){if(device_control_scheduler_is_idle(id))return ESP_OK;vTaskDelay(pdMS_TO_TICKS(10));} return ESP_ERR_TIMEOUT; }
void device_control_scheduler_get_stats(device_control_scheduler_stats_t *out) { if(!out)return; taskENTER_CRITICAL(&g_ctrl.mux); *out=g_ctrl.stats; taskEXIT_CRITICAL(&g_ctrl.mux); }
