#include "device_command_service_internal.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static void complete_event(const dcs_event_t *event, device_command_status_t status)
{
    device_command_result_t result = { .status = status };
    if (event->completion != NULL) {
        event->completion(&result, event->context);
    }
}

static void handle_submit(const dcs_event_t *event)
{
    const device_command_request_t *request = &event->request;
    device_command_status_t validation = dcs_validate_request(request);
    if (validation != DEVICE_CMD_STATUS_OK) {
        complete_event(event, validation);
        return;
    }
    if (request->origin == DEVICE_CMD_ORIGIN_CONTROL &&
        g_dcs.hooks.is_connected(request->device_id) <= 0) {
        complete_event(event, DEVICE_CMD_STATUS_NOT_CONNECTED);
        return;
    }
    if (dcs_pending_find_device(request->device_id) != NULL) {
        dcs_stats_inc(&g_dcs.stats.busy_rejections);
        complete_event(event, DEVICE_CMD_STATUS_BUSY);
        return;
    }
    dcs_pending_slot_t *slot = dcs_pending_allocate();
    if (slot == NULL) {
        dcs_stats_inc(&g_dcs.stats.queue_full);
        complete_event(event, DEVICE_CMD_STATUS_QUEUE_FULL);
        return;
    }

    slot->in_use = true;
    slot->request_id = dcs_pending_next_request_id();
    strlcpy(slot->device_id, request->device_id, sizeof(slot->device_id));
    strlcpy(slot->command, request->command, sizeof(slot->command));
    slot->origin = request->origin;
    slot->completion = event->completion;
    slot->context = event->context;
    slot->deadline_us = esp_timer_get_time() + DCS_ACK_TIMEOUT_MS * 1000LL;
    slot->has_bool_value = request->has_bool_value;
    slot->bool_value = request->bool_value;
    slot->has_int_value = request->has_int_value;
    slot->int_value = request->int_value;
    slot->has_feature_id = request->has_feature_id;
    if (request->has_feature_id) {
        strlcpy(slot->feature_id, request->feature_id, sizeof(slot->feature_id));
    }
    slot->has_property_id = request->has_property_id;
    slot->property_id = request->property_id;

    uint32_t count = dcs_pending_count();
    taskENTER_CRITICAL(&g_dcs.stats_mux);
    if (count > g_dcs.stats.max_pending) {
        g_dcs.stats.max_pending = count;
    }
    taskEXIT_CRITICAL(&g_dcs.stats_mux);

    gw_message_t wire_message;
    dcs_build_wire_message(request, slot->request_id, &wire_message);
    ESP_LOGI(DCS_TAG, "[SEND] device=%s request_id=%lu command=%s origin=%d",
             slot->device_id, (unsigned long)slot->request_id,
             slot->command, slot->origin);
    if (g_dcs.hooks.send_command(slot->device_id, &wire_message) != 0) {
        ESP_LOGW(DCS_TAG, "[SEND_FAILED] device=%s request_id=%lu",
                 slot->device_id, (unsigned long)slot->request_id);
        dcs_stats_inc(&g_dcs.stats.transport_errors);
        dcs_pending_complete_status(slot, DEVICE_CMD_STATUS_TRANSPORT_ERROR);
        return;
    }
    dcs_stats_inc(&g_dcs.stats.submitted);
}

static void handle_ack(const dcs_event_t *event)
{
    const gw_message_t *message = &event->ack_message;
    if (!message->has_request_id || message->request_id == 0) {
        return;
    }
    dcs_pending_slot_t *slot =
        dcs_pending_find_id(event->ack_device_id, message->request_id);
    if (slot == NULL) {
        ESP_LOGI(DCS_TAG, "[ACK_UNMATCHED] device=%s request_id=%lu",
                 event->ack_device_id, (unsigned long)message->request_id);
        return;
    }
    if (strcmp(slot->command, message->command) != 0) {
        ESP_LOGW(DCS_TAG, "[ACK_CMD_MISMATCH] device=%s request_id=%lu expected=%s got=%s",
                 event->ack_device_id, (unsigned long)message->request_id,
                 slot->command, message->command);
        return;
    }

    ESP_LOGI(DCS_TAG, "[ACK] device=%s request_id=%lu command=%s accepted=%d",
             event->ack_device_id, (unsigned long)message->request_id,
             slot->command, message->bool_value);
    device_command_result_t result = { .request_id = slot->request_id };
    if (message->bool_value) {
        result.status = DEVICE_CMD_STATUS_OK;
        result.accepted = true;
        if (message->has_bool_value) {
            result.has_bool_value = true;
            result.bool_value = message->bool_value != 0;
        }
        if (message->has_int_value) {
            result.has_int_value = true;
            result.int_value = message->int_value;
        }
        if (message->has_feature_value_bool) {
            result.has_feature_value_bool = true;
            result.feature_value_bool = message->feature_value_bool;
        }
        if (message->has_feature_value_int) {
            result.has_feature_value_int = true;
            result.feature_value_int = message->feature_value_int;
        }
        dcs_stats_inc(&g_dcs.stats.completed_ok);
    } else {
        result.status = DEVICE_CMD_STATUS_DEVICE_REJECTED;
        result.accepted = false;
        dcs_stats_inc(&g_dcs.stats.completed_error);
    }
    dcs_pending_complete(slot, &result);
}

static void handle_disconnect(const dcs_event_t *event)
{
    dcs_pending_slot_t *slot =
        dcs_pending_find_device(event->disconnect_device_id);
    if (slot != NULL) {
        ESP_LOGI(DCS_TAG, "[DISCONNECT] device=%s request_id=%lu",
                 event->disconnect_device_id, (unsigned long)slot->request_id);
        dcs_stats_inc(&g_dcs.stats.disconnect_count);
        dcs_pending_complete_status(slot, DEVICE_CMD_STATUS_NOT_CONNECTED);
    }
}

static void handle_cancel(const dcs_event_t *event)
{
    dcs_pending_slot_t *slot = dcs_pending_find_device(event->cancel_device_id);
    if (slot != NULL) {
        ESP_LOGI(DCS_TAG, "[CANCEL] device=%s request_id=%lu",
                 event->cancel_device_id, (unsigned long)slot->request_id);
        dcs_pending_complete_status(slot, DEVICE_CMD_STATUS_CANCELLED);
    }
}

static void check_timeouts(void)
{
    int64_t now_us = esp_timer_get_time();
    for (size_t i = 0; i < DCS_MAX_PENDING; i++) {
        if (g_dcs.pending[i].in_use && now_us >= g_dcs.pending[i].deadline_us) {
            ESP_LOGW(DCS_TAG, "[TIMEOUT] device=%s request_id=%lu",
                     g_dcs.pending[i].device_id,
                     (unsigned long)g_dcs.pending[i].request_id);
            dcs_stats_inc(&g_dcs.stats.timeout_count);
            dcs_pending_complete_status(&g_dcs.pending[i],
                                        DEVICE_CMD_STATUS_TIMEOUT);
        }
    }
}

void dcs_service_task(void *arg)
{
    (void)arg;
    dcs_event_t event;
    while (g_dcs.running) {
        int64_t nearest_deadline_us = esp_timer_get_time() + 1000000LL;
        for (size_t i = 0; i < DCS_MAX_PENDING; i++) {
            if (g_dcs.pending[i].in_use &&
                g_dcs.pending[i].deadline_us < nearest_deadline_us) {
                nearest_deadline_us = g_dcs.pending[i].deadline_us;
            }
        }
        int64_t wait_us = nearest_deadline_us - esp_timer_get_time();
        TickType_t wait_ticks = wait_us > 0 ? pdMS_TO_TICKS(wait_us / 1000) : 0;
        if (xQueueReceive(g_dcs.queue, &event, wait_ticks) == pdTRUE) {
            switch (event.type) {
            case DCS_EVENT_SUBMIT:
                handle_submit(&event);
                break;
            case DCS_EVENT_ACK:
                handle_ack(&event);
                break;
            case DCS_EVENT_DISCONNECT:
                handle_disconnect(&event);
                break;
            case DCS_EVENT_CANCEL:
                handle_cancel(&event);
                break;
            case DCS_EVENT_SHUTDOWN:
                g_dcs.running = false;
                break;
            }
        }
        check_timeouts();
    }
    for (size_t i = 0; i < DCS_MAX_PENDING; i++) {
        if (g_dcs.pending[i].in_use) {
            dcs_pending_complete_status(&g_dcs.pending[i],
                                        DEVICE_CMD_STATUS_CANCELLED);
        }
    }
    ESP_LOGI(DCS_TAG, "Service task stopped");
    g_dcs.task_stopped = true;
    vTaskDelete(NULL);
}
