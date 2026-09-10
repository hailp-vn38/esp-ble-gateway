#include "device_settings_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define DS_EVENT_QUEUE_LEN 24
static QueueHandle_t s_queue;
static ds_actor_metrics_t s_metrics;

esp_err_t ds_events_init(void)
{
    if (s_queue != NULL) return ESP_OK;
    s_queue = xQueueCreate(DS_EVENT_QUEUE_LEN, sizeof(ds_event_t));
    return s_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}
void ds_events_deinit(void) { if (s_queue != NULL) { vQueueDelete(s_queue); s_queue = NULL; } }
bool ds_events_post(const ds_event_t *event)
{
    if (event == NULL || s_queue == NULL || xQueueSend(s_queue, event, 0) != pdTRUE) {
        s_metrics.event_queue_full++;
        return false;
    }
    return true;
}
bool ds_events_wait(ds_event_t *event)
{ return event != NULL && s_queue != NULL && xQueueReceive(s_queue, event, portMAX_DELAY) == pdTRUE; }
void ds_events_get_metrics(ds_actor_metrics_t *out) { if (out != NULL) *out = s_metrics; }
