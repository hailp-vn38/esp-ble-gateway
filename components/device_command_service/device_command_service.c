#include "device_command_service_internal.h"

#include <string.h>

#include "esp_log.h"

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
    if (g_dcs.queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    dcs_pending_reset();
    memset(&g_dcs.stats, 0, sizeof(g_dcs.stats));
    g_dcs.queue = xQueueCreate(DCS_QUEUE_LEN, sizeof(dcs_event_t));
    if (g_dcs.queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    g_dcs.running = true;
    BaseType_t created = xTaskCreate(dcs_service_task, "dev_cmd_svc",
                                     DCS_TASK_STACK, NULL,
                                     DCS_TASK_PRIORITY, &g_dcs.task);
    if (created != pdPASS) {
        vQueueDelete(g_dcs.queue);
        g_dcs.queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(DCS_TAG, "Device command service started (queue=%d, pending=%d)",
             DCS_QUEUE_LEN, DCS_MAX_PENDING);
    return ESP_OK;
}

void device_command_service_deinit(void)
{
    if (g_dcs.queue == NULL) {
        return;
    }
    dcs_event_t shutdown = { .type = DCS_EVENT_SHUTDOWN };
    xQueueSend(g_dcs.queue, &shutdown, pdMS_TO_TICKS(100));
    for (int wait_ms = 0;
         wait_ms < DCS_DEINIT_WAIT_BUDGET_MS && g_dcs.running;
         wait_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vQueueDelete(g_dcs.queue);
    g_dcs.queue = NULL;
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
    if (g_dcs.queue == NULL || !g_dcs.running) {
        return ESP_ERR_INVALID_STATE;
    }
    dcs_event_t event = {
        .type = DCS_EVENT_SUBMIT,
        .request = *request,
        .completion = completion,
        .context = context,
    };
    if (xQueueSend(g_dcs.queue, &event, 0) != pdTRUE) {
        dcs_stats_inc(&g_dcs.stats.queue_full);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool device_command_service_on_notify(const char *device_id,
                                      const gw_message_t *message)
{
    if (device_id == NULL || message == NULL ||
        strcmp(message->type, "device_ack") != 0 ||
        !message->has_request_id || message->request_id == 0) {
        return false;
    }
    dcs_event_t event = { .type = DCS_EVENT_ACK };
    strlcpy(event.ack_device_id, device_id, sizeof(event.ack_device_id));
    event.ack_message = *message;
    if (xQueueSend(g_dcs.queue, &event, 0) != pdTRUE) {
        ESP_LOGW(DCS_TAG, "ACK event queue full for device=%s", device_id);
        return false;
    }
    return true;
}

void device_command_service_on_disconnect(const char *device_id)
{
    if (device_id == NULL) {
        return;
    }
    dcs_event_t event = { .type = DCS_EVENT_DISCONNECT };
    strlcpy(event.disconnect_device_id, device_id,
            sizeof(event.disconnect_device_id));
    xQueueSend(g_dcs.queue, &event, pdMS_TO_TICKS(100));
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
