#include "gateway_events.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "gw_events";

/* ── State ─────────────────────────────────────────────────────────── */

static uint32_t s_seq;
static SemaphoreHandle_t s_mutex;

typedef struct {
    gateway_event_listener_t fn;
    void *context;
    bool in_use;
} listener_slot_t;

static listener_slot_t s_listeners[GATEWAY_EVENT_MAX_LISTENERS];

/* ── Init ──────────────────────────────────────────────────────────── */

esp_err_t gateway_events_init(void)
{
    s_seq = 0;
    memset(s_listeners, 0, sizeof(s_listeners));

    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "mutex creation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "gateway_events initialized (max %d listeners)",
             GATEWAY_EVENT_MAX_LISTENERS);
    return ESP_OK;
}

/* ── Register ──────────────────────────────────────────────────────── */

esp_err_t gateway_events_register(gateway_event_listener_t listener,
                                  void *context)
{
    if (listener == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000));

    for (int i = 0; i < GATEWAY_EVENT_MAX_LISTENERS; i++) {
        if (!s_listeners[i].in_use) {
            s_listeners[i].fn = listener;
            s_listeners[i].context = context;
            s_listeners[i].in_use = true;
            xSemaphoreGive(s_mutex);
            ESP_LOGD(TAG, "listener registered in slot %d", i);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "listener registration failed: no free slot");
    return ESP_ERR_NO_MEM;
}

/* ── Publish ───────────────────────────────────────────────────────── */

void gateway_events_publish(gateway_event_t *event)
{
    if (event == NULL) {
        return;
    }

    /* If not initialized, silently drop */
    if (s_mutex == NULL) {
        return;
    }

    /* Assign monotonic sequence under lock */
    xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000));
    event->seq = ++s_seq;

    /* Copy listener list to avoid holding lock during callbacks */
    listener_slot_t local[GATEWAY_EVENT_MAX_LISTENERS];
    memcpy(local, s_listeners, sizeof(local));
    xSemaphoreGive(s_mutex);

    /* Fan-out without holding any lock */
    for (int i = 0; i < GATEWAY_EVENT_MAX_LISTENERS; i++) {
        if (local[i].in_use && local[i].fn != NULL) {
            local[i].fn(event, local[i].context);
        }
    }
}

/* ── Current seq ───────────────────────────────────────────────────── */

uint32_t gateway_events_current_seq(void)
{
    if (s_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000));
    uint32_t current = s_seq;
    xSemaphoreGive(s_mutex);
    return current;
}

/* ── Reset (test only) ────────────────────────────────────────────── */

void gateway_events_reset_for_test(void)
{
    if (s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000));
    s_seq = 0;
    memset(s_listeners, 0, sizeof(s_listeners));
    xSemaphoreGive(s_mutex);
}
