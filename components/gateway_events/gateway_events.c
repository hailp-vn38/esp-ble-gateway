#include "gateway_events.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "gw_events";

/* ── State ─────────────────────────────────────────────────────────── */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static uint32_t s_seq;

typedef struct {
    gateway_event_listener_t fn;
    void *context;
    bool in_use;
} listener_slot_t;

static listener_slot_t s_listeners[GATEWAY_EVENT_MAX_LISTENERS];

/* ── Init ──────────────────────────────────────────────────────────── */

esp_err_t gateway_events_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_seq = 0;
    memset(s_listeners, 0, sizeof(s_listeners));
    s_initialized = true;

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

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);

    for (int i = 0; i < GATEWAY_EVENT_MAX_LISTENERS; i++) {
        if (!s_listeners[i].in_use) {
            s_listeners[i].fn = listener;
            s_listeners[i].context = context;
            s_listeners[i].in_use = true;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGD(TAG, "listener registered in slot %d", i);
            return ESP_OK;
        }
    }

    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "listener registration failed: no free slot");
    return ESP_ERR_NO_MEM;
}

/* ── Publish ───────────────────────────────────────────────────────── */

void gateway_events_publish(gateway_event_t *event)
{
    if (event == NULL) {
        return;
    }

    if (!s_initialized) {
        return;
    }

    listener_slot_t local[GATEWAY_EVENT_MAX_LISTENERS];

    portENTER_CRITICAL(&s_lock);

    event->seq = ++s_seq;
    memcpy(local, s_listeners, sizeof(local));

    portEXIT_CRITICAL(&s_lock);

    for (int i = 0; i < GATEWAY_EVENT_MAX_LISTENERS; i++) {
        if (local[i].in_use && local[i].fn != NULL) {
            local[i].fn(event, local[i].context);
        }
    }
}

/* ── Current seq ───────────────────────────────────────────────────── */

uint32_t gateway_events_current_seq(void)
{
    if (!s_initialized) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    uint32_t current = s_seq;
    portEXIT_CRITICAL(&s_lock);
    return current;
}

/* ── Reset (test only) ────────────────────────────────────────────── */

void gateway_events_reset_for_test(void)
{
    if (!s_initialized) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_seq = 0;
    memset(s_listeners, 0, sizeof(s_listeners));
    portEXIT_CRITICAL(&s_lock);
}
