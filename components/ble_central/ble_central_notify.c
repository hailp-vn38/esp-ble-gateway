#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "ble_central_internal.h"
#include "cbor_codec.h"

static const char *TAG = "ble_central_notify";

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint8_t cbor_buf[GW_MSG_MAX_LEN];
    uint16_t cbor_len;
} ble_notify_event_t;

static QueueHandle_t s_notify_queue;
static TaskHandle_t s_notify_task;
static ble_central_notify_cb_t s_notify_cb;
static volatile uint32_t s_notify_queue_high_watermark;

uint32_t ble_central_notify_queue_high_watermark(void)
{
    return __atomic_load_n(&s_notify_queue_high_watermark, __ATOMIC_RELAXED);
}

int ble_central_notify_init(ble_central_notify_cb_t notify_cb)
{
    s_notify_cb = notify_cb;
    if (s_notify_queue == NULL) {
        s_notify_queue = xQueueCreate(BLE_NOTIFY_QUEUE_DEPTH,
                                      sizeof(ble_notify_event_t));
    }
    if (s_notify_queue == NULL) return -1;

    if (s_notify_task == NULL &&
        xTaskCreate(ble_central_notify_worker, "ble_notify",
                    BLE_NOTIFY_TASK_STACK, NULL, BLE_NOTIFY_TASK_PRIORITY,
                    &s_notify_task) != pdPASS) {
        return -1;
    }
    return 0;
}

void ble_central_notify_worker(void *arg)
{
    (void)arg;
    ble_notify_event_t event;

    for (;;) {
        if (xQueueReceive(s_notify_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        gw_message_t message;
        if (cbor_codec_decode(event.cbor_buf, event.cbor_len, &message) != 0) {
            ESP_LOGE(TAG, "[%s] Invalid CBOR notify", event.device_id);
            ble_central_metrics_notify_decode_error();
            continue;
        }

        ble_central_notify_cb_t cb = s_notify_cb;
        if (cb != NULL) cb(event.device_id, &message);
    }
}

void ble_central_notify_enqueue(const char *device_id, const uint8_t *data,
                                uint16_t len)
{
    if (device_id == NULL || data == NULL || len == 0 ||
        len > GW_MSG_MAX_LEN) {
        return;
    }

    ble_central_metrics_notify_received();

    ble_notify_event_t event;
    memset(&event, 0, sizeof(event));
    strlcpy(event.device_id, device_id, sizeof(event.device_id));
    memcpy(event.cbor_buf, data, len);
    event.cbor_len = len;

    if (xQueueSend(s_notify_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "[%s] Notify queue full, dropping (%u bytes)", device_id,
                 len);
        ble_central_metrics_notify_dropped();
        return;
    }

    ble_central_metrics_notify_enqueued();
    uint32_t depth = (uint32_t)uxQueueMessagesWaiting(s_notify_queue);
    uint32_t previous = __atomic_load_n(&s_notify_queue_high_watermark,
                                        __ATOMIC_RELAXED);
    while (depth > previous &&
           !__atomic_compare_exchange_n(&s_notify_queue_high_watermark,
                                        &previous, depth, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}
