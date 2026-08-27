#include "message_trace.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "msg_trace";

static atomic_uint s_next_frame_id = ATOMIC_VAR_INIT(0);

uint32_t message_trace_next_frame_id(void)
{
    uint32_t id;
    do {
        id = atomic_fetch_add(&s_next_frame_id, 1) + 1;
    } while (id == 0);
    return id;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void message_trace_tx_decoded(uint32_t frame_id,
                              const char *device_id,
                              const gw_message_t *message,
                              size_t encoded_len)
{
    if (message == NULL) return;
    ESP_LOGI(TAG, "[MSG_TX] frame=%lu device=%s type=%s command=%s "
                  "request=%lu len=%u",
             (unsigned long)frame_id,
             device_id ? device_id : "?",
             message->type,
             message->command,
             (unsigned long)message->request_id,
             (unsigned)encoded_len);
}

void message_trace_tx_raw(uint32_t frame_id,
                          const uint8_t *data,
                          size_t len)
{
    if (data == NULL || len == 0) return;
    /* Log first 32 bytes as hex for debugging. */
    char hex[65] = {0};
    size_t hex_len = len < 32 ? len : 32;
    for (size_t i = 0; i < hex_len; i++) {
        snprintf(hex + i * 2, 3, "%02x", data[i]);
    }
    ESP_LOGD(TAG, "[MSG_TX_RAW] frame=%lu len=%u hex=%s%s",
             (unsigned long)frame_id, (unsigned)len, hex,
             len > 32 ? "..." : "");
}

void message_trace_tx_result(uint32_t frame_id, int result)
{
    ESP_LOGI(TAG, "[MSG_TX_RESULT] frame=%lu rc=%d", (unsigned long)frame_id,
             result);
}

void message_trace_rx_raw(uint32_t frame_id,
                          const char *device_id,
                          const uint8_t *data,
                          size_t len)
{
    if (data == NULL || len == 0) return;
    char hex[65] = {0};
    size_t hex_len = len < 32 ? len : 32;
    for (size_t i = 0; i < hex_len; i++) {
        snprintf(hex + i * 2, 3, "%02x", data[i]);
    }
    ESP_LOGD(TAG, "[MSG_RX_RAW] frame=%lu device=%s len=%u hex=%s%s",
             (unsigned long)frame_id,
             device_id ? device_id : "?",
             (unsigned)len, hex,
             len > 32 ? "..." : "");
}

void message_trace_rx_decoded(uint32_t frame_id,
                              const char *device_id,
                              const gw_message_t *message)
{
    if (message == NULL) return;
    ESP_LOGI(TAG, "[MSG_RX] frame=%lu device=%s type=%s command=%s "
                  "request=%lu",
             (unsigned long)frame_id,
             device_id ? device_id : "?",
             message->type,
             message->command,
             (unsigned long)message->request_id);
}

void message_trace_rx_decode_error(uint32_t frame_id,
                                   const char *device_id,
                                   int decode_result)
{
    ESP_LOGW(TAG, "[MSG_RX_DECODE_ERROR] frame=%lu device=%s rc=%d",
             (unsigned long)frame_id,
             device_id ? device_id : "?",
             decode_result);
}
