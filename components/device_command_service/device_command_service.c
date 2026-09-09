#include "device_command_service_internal.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

const char *DCS_TAG = "dev_cmd_svc";

static int default_send_command(const char *device_id, const gw_message_t *message);
static int default_is_connected(const char *device_id);

dcs_state_t g_dcs = {
    .stats_mux = portMUX_INITIALIZER_UNLOCKED,
    .hooks = {
        .send_command = default_send_command,
        .is_connected = default_is_connected,
    },
};

void dcs_stats_inc(uint32_t *field)
{
    taskENTER_CRITICAL(&g_dcs.stats_mux);
    (*field)++;
    taskEXIT_CRITICAL(&g_dcs.stats_mux);
}

static int default_send_command(const char *device_id, const gw_message_t *message)
{
    extern int ble_central_send_command(const char *device_id,
                                        const gw_message_t *message);
    return ble_central_send_command(device_id, message);
}

static int default_is_connected(const char *device_id)
{
    extern int ble_central_is_connected(const char *device_id);
    return ble_central_is_connected(device_id);
}

esp_err_t device_command_service_init(void)
{
    if (g_dcs.normal_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    dcs_pending_reset();
    memset(&g_dcs.stats, 0, sizeof(g_dcs.stats));
    g_dcs.normal_queue = xQueueCreate(DCS_QUEUE_LEN, sizeof(dcs_submit_event_t));
    g_dcs.urgent_queue = xQueueCreate(DCS_URGENT_QUEUE_LEN,
                                      sizeof(dcs_urgent_event_t));
    if (g_dcs.normal_queue == NULL || g_dcs.urgent_queue == NULL) {
        if (g_dcs.normal_queue != NULL) vQueueDelete(g_dcs.normal_queue);
        if (g_dcs.urgent_queue != NULL) vQueueDelete(g_dcs.urgent_queue);
        g_dcs.normal_queue = NULL;
        g_dcs.urgent_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    g_dcs.running = true;
    g_dcs.task_stopped = false;
    BaseType_t created = xTaskCreate(dcs_service_task, "dev_cmd_svc",
                                     DCS_TASK_STACK, NULL,
                                     DCS_TASK_PRIORITY, &g_dcs.task);
    if (created != pdPASS) {
        vQueueDelete(g_dcs.normal_queue);
        vQueueDelete(g_dcs.urgent_queue);
        g_dcs.normal_queue = NULL;
        g_dcs.urgent_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(DCS_TAG, "Device command service started (normal=%d urgent=%d pending=%d)",
             DCS_QUEUE_LEN, DCS_URGENT_QUEUE_LEN, DCS_MAX_PENDING);
    return ESP_OK;
}

void device_command_service_deinit(void)
{
    if (g_dcs.normal_queue == NULL) {
        return;
    }
    dcs_urgent_event_t shutdown = { .type = DCS_URGENT_EVENT_SHUTDOWN };
    if (!dcs_urgent_enqueue(&shutdown, pdMS_TO_TICKS(100))) {
        /* Do not delete queues below a still-running task if a pathological
         * urgent burst prevents the shutdown event from being queued. */
        g_dcs.running = false;
        xTaskNotifyGive(g_dcs.task);
    }
    for (int wait_ms = 0;
         wait_ms < DCS_DEINIT_WAIT_BUDGET_MS && !g_dcs.task_stopped;
         wait_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vQueueDelete(g_dcs.normal_queue);
    vQueueDelete(g_dcs.urgent_queue);
    g_dcs.normal_queue = NULL;
    g_dcs.urgent_queue = NULL;
    g_dcs.task = NULL;
    ESP_LOGI(DCS_TAG, "Device command service stopped");
}

esp_err_t device_command_service_submit(const device_command_request_t *request,
                                        device_command_completion_fn completion,
                                        void *context)
{
    if (request == NULL || completion == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_dcs.normal_queue == NULL || !g_dcs.running) {
        return ESP_ERR_INVALID_STATE;
    }
    dcs_submit_event_t event = {
        .request = *request,
        .completion = completion,
        .context = context,
    };
    if (xQueueSend(g_dcs.normal_queue, &event, 0) != pdTRUE) {
        dcs_stats_inc(&g_dcs.stats.queue_full);
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(g_dcs.task);
    return ESP_OK;
}

bool device_command_service_on_notify(const char *device_id,
                                      const gw_message_t *message)
{
    if (device_id == NULL || message == NULL ||
        strcmp(message->type, "device_ack") != 0 ||
        !message->has_request_id || message->request_id == 0 ||
        g_dcs.urgent_queue == NULL || !g_dcs.running) {
        return false;
    }
    dcs_urgent_event_t event = {
        .type = DCS_URGENT_EVENT_ACK,
        .enqueued_us = esp_timer_get_time(),
        .data.ack = {
            .request_id = message->request_id,
            .accepted = message->bool_value != 0,
            .has_bool_value = message->has_bool_value,
            .bool_value = message->bool_value != 0,
            .has_int_value = message->has_int_value,
            .int_value = message->int_value,
            .has_feature_value_bool = message->has_feature_value_bool,
            .feature_value_bool = message->feature_value_bool,
            .has_feature_value_int = message->has_feature_value_int,
            .feature_value_int = message->feature_value_int,
        },
    };
    strlcpy(event.data.ack.device_id, device_id, sizeof(event.data.ack.device_id));
    strlcpy(event.data.ack.command, message->command, sizeof(event.data.ack.command));
    if (!dcs_urgent_enqueue(&event, 0)) {
        ESP_LOGW(DCS_TAG, "ACK urgent queue full for device=%s", device_id);
        return false;
    }
    return true;
}

void device_command_service_on_disconnect(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0' ||
        g_dcs.urgent_queue == NULL || !g_dcs.running) {
        return;
    }
    dcs_urgent_event_t event = { .type = DCS_URGENT_EVENT_DISCONNECT };
    strlcpy(event.data.device_id, device_id, sizeof(event.data.device_id));
    dcs_urgent_enqueue(&event, pdMS_TO_TICKS(100));
}

esp_err_t device_command_service_cancel_device(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_dcs.urgent_queue == NULL || !g_dcs.running) {
        return ESP_ERR_INVALID_STATE;
    }
    dcs_urgent_event_t event = { .type = DCS_URGENT_EVENT_CANCEL };
    strlcpy(event.data.device_id, device_id, sizeof(event.data.device_id));
    return dcs_urgent_enqueue(&event, 0)
               ? ESP_OK : ESP_ERR_NO_MEM;
}

void device_command_service_get_stats(device_command_service_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&g_dcs.stats_mux);
    *out = g_dcs.stats;
    taskEXIT_CRITICAL(&g_dcs.stats_mux);
}

void device_command_service_set_hooks(const device_command_transport_hooks_t *hooks)
{
    if (hooks == NULL) {
        g_dcs.hooks.send_command = default_send_command;
        g_dcs.hooks.is_connected = default_is_connected;
    } else {
        g_dcs.hooks = *hooks;
    }
}

uint32_t device_command_service_get_pending_count(void)
{
    return dcs_pending_count();
}
