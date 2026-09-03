#include "device_schema_internal.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "memory_policy.h"

#define SCHEMA_EVENT_QUEUE_DEPTH 32
#define SCHEMA_WORKER_STACK 6144
#define SCHEMA_WORKER_PRIORITY (tskIDLE_PRIORITY + 3)

static const char *TAG = "schema_worker";

/* ── Module state ───────────────────────────────────────────────────── */

static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static volatile bool s_shutdown;

/* Queue health metrics */
static uint32_t s_q_enqueued;
static uint32_t s_q_dropped;
static uint32_t s_q_high_watermark;
static uint32_t s_q_message_alloc_fail;
static int64_t s_last_drop_log_us;

/* ── Worker task ────────────────────────────────────────────────────── */

static void schema_worker_task(void *arg)
{
    (void)arg;
    schema_event_t event;
    for (;;) {
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (s_shutdown) break;
            continue;
        }
        if (s_shutdown) break;
        switch (event.type) {
        case SCHEMA_EVENT_READY:
            schema_discovery_handle_ready(event.device_id);
            break;
        case SCHEMA_EVENT_REFRESH:
            schema_discovery_start(event.device_id, SCHEMA_OP_MANUAL,
                                   event.refresh_generation);
            break;
        case SCHEMA_EVENT_DISCONNECT:
            schema_discovery_handle_disconnect(event.device_id);
            break;
        case SCHEMA_EVENT_NOTIFY:
            if (event.message != NULL) {
                schema_protocol_handle_message(event.device_id, event.message);
                gw_mem_free(event.message);
            }
            break;
        case SCHEMA_EVENT_COMPLETION:
            schema_discovery_handle_completion(event.device_id,
                                               event.operation_id,
                                               event.refresh_generation,
                                               event.completion);
            break;
        }
    }
    for (;;) { vTaskDelay(portMAX_DELAY); }
}

/* ── Internal API ───────────────────────────────────────────────────── */

esp_err_t schema_worker_init(void)
{
    if (s_queue == NULL) {
        s_queue = xQueueCreate(SCHEMA_EVENT_QUEUE_DEPTH, sizeof(schema_event_t));
        if (s_queue == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_worker == NULL) {
        if (xTaskCreate(schema_worker_task, "device_schema", SCHEMA_WORKER_STACK,
                        NULL, SCHEMA_WORKER_PRIORITY, &s_worker) != pdPASS) {
            vQueueDelete(s_queue);
            s_queue = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t schema_worker_post_event(const schema_event_t *event)
{
    if (s_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_queue, event, 0) != pdTRUE) {
        __atomic_fetch_add(&s_q_dropped, 1, __ATOMIC_RELAXED);
        return ESP_ERR_NO_MEM;
    }
    __atomic_fetch_add(&s_q_enqueued, 1, __ATOMIC_RELAXED);

    UBaseType_t depth = uxQueueMessagesWaiting(s_queue);
    uint32_t prev = __atomic_load_n(&s_q_high_watermark, __ATOMIC_RELAXED);
    while ((uint32_t)depth > prev &&
           !__atomic_compare_exchange_n(&s_q_high_watermark, &prev,
                                        (uint32_t)depth, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    return ESP_OK;
}

bool schema_worker_post_notify(const char *device_id,
                               const gw_message_t *message)
{
    if (message == NULL ||
        (strcmp(message->type, "capabilities_begin") != 0 &&
         strcmp(message->type, "capability_item") != 0 &&
         strcmp(message->type, "feature_item") != 0 &&
         strcmp(message->type, "capabilities_end") != 0)) {
        return false;
    }
    if (!schema_runtime_is_initialized() || device_id == NULL) return true;

    uint32_t op_id = 0;
    if (schema_runtime_lock()) {
        schema_record_t *record = schema_runtime_find_locked(device_id);
        if (record != NULL) {
            op_id = record->operation_id;
        }
        schema_runtime_unlock();
    }

    schema_event_t event = {
        .type = SCHEMA_EVENT_NOTIFY,
        .operation_id = op_id,
    };
    strlcpy(event.device_id, device_id, sizeof(event.device_id));

    event.message = gw_mem_alloc(sizeof(*event.message),
                                GW_MEM_EXTERNAL_PREFERRED);
    if (event.message == NULL) {
        __atomic_fetch_add(&s_q_message_alloc_fail, 1, __ATOMIC_RELAXED);
        ESP_LOGE(TAG, "[%s] schema message alloc failed", device_id);
        return true;
    }
    *event.message = *message;

    if (xQueueSend(s_queue, &event, 0) != pdTRUE) {
        gw_mem_free(event.message);
        __atomic_fetch_add(&s_q_dropped, 1, __ATOMIC_RELAXED);
        int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_drop_log_us > 5000000LL) {
            s_last_drop_log_us = now_us;
            ESP_LOGW(TAG, "[%s] schema queue full (dropped total: %u)",
                     device_id,
                     (unsigned)__atomic_load_n(&s_q_dropped,
                                               __ATOMIC_RELAXED));
        }
        return true;
    }

    __atomic_fetch_add(&s_q_enqueued, 1, __ATOMIC_RELAXED);
    UBaseType_t depth = uxQueueMessagesWaiting(s_queue);
    uint32_t prev = __atomic_load_n(&s_q_high_watermark, __ATOMIC_RELAXED);
    while ((uint32_t)depth > prev &&
           !__atomic_compare_exchange_n(&s_q_high_watermark, &prev,
                                        (uint32_t)depth, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    return true;
}

void schema_worker_get_stats(device_schema_queue_stats_t *out)
{
    if (out == NULL) return;
    out->enqueued =
        __atomic_load_n(&s_q_enqueued, __ATOMIC_RELAXED);
    out->dropped =
        __atomic_load_n(&s_q_dropped, __ATOMIC_RELAXED);
    out->high_watermark =
        __atomic_load_n(&s_q_high_watermark, __ATOMIC_RELAXED);
    out->message_alloc_fail =
        __atomic_load_n(&s_q_message_alloc_fail, __ATOMIC_RELAXED);
}

void schema_worker_reset_for_test(void)
{
    if (s_queue != NULL) {
        schema_event_t event;
        while (xQueueReceive(s_queue, &event, 0) == pdTRUE) {
            if (event.type == SCHEMA_EVENT_NOTIFY && event.message != NULL) {
                gw_mem_free(event.message);
            }
        }
    }
}
