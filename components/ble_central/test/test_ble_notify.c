#include <string.h>

#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_central_internal.h"
#include "cbor_codec.h"

#include "test_ble_common.h"

static uint16_t make_valid_payload(uint8_t *out, size_t cap)
{
    gw_message_t msg = {0};
    msg.protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(msg.type, "cmd", sizeof(msg.type));
    strlcpy(msg.device_id, "notify-target", sizeof(msg.device_id));
    strlcpy(msg.command, "toggle", sizeof(msg.command));
    msg.has_device_id = 1;
    int length = cbor_codec_encode(&msg, out, cap);
    TEST_ASSERT_GREATER_THAN_INT(0, length);
    return (uint16_t)length;
}

TEST_CASE("notify enqueue rejects invalid payloads", "[ble_notify]")
{
    ble_test_bootstrap();

    uint32_t received_before = ble_central_metrics()->notify_received;

    ble_central_notify_enqueue("dev-x", NULL, 10);
    uint8_t payload[GW_MSG_MAX_LEN] = {0};
    ble_central_notify_enqueue("dev-x", payload, 0);
    ble_central_notify_enqueue("dev-x", payload, GW_MSG_MAX_LEN + 1);

    TEST_ASSERT_EQUAL_UINT32(received_before,
                             ble_central_metrics()->notify_received);
}

TEST_CASE("notify queue overflow drops without blocking", "[ble_notify]")
{
    ble_test_bootstrap();

    uint8_t payload[GW_MSG_MAX_LEN];
    uint16_t len = make_valid_payload(payload, sizeof(payload));

    uint32_t received_before = ble_central_metrics()->notify_received;
    uint32_t enqueued_before = ble_central_metrics()->notify_enqueued;
    uint32_t dropped_before = ble_central_metrics()->notify_dropped;

    for (int i = 0; i < BLE_NOTIFY_QUEUE_DEPTH + 4; i++) {
        ble_central_notify_enqueue("dev-flood", payload, len);
    }

    const uint32_t sent = BLE_NOTIFY_QUEUE_DEPTH + 4;
    TEST_ASSERT_EQUAL_UINT32(received_before + sent,
                             ble_central_metrics()->notify_received);
    TEST_ASSERT_EQUAL_UINT32(
        enqueued_before + dropped_before + sent,
        ble_central_metrics()->notify_enqueued +
            ble_central_metrics()->notify_dropped);

    vTaskDelay(pdMS_TO_TICKS(200));
}

TEST_CASE("invalid CBOR notify increments decode errors", "[ble_notify]")
{
    ble_test_bootstrap();

    uint32_t decode_errors_before =
        ble_central_metrics()->notify_decode_errors;

    const uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF};
    ble_central_notify_enqueue("dev-garbage", garbage, sizeof(garbage));

    vTaskDelay(pdMS_TO_TICKS(200));

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(
        decode_errors_before + 1,
        ble_central_metrics()->notify_decode_errors);
}
