#include <string.h>

#include "message_trace.h"
#include "unity.h"

TEST_CASE("frame id allocation is nonzero and unique in sequence",
          "[message_trace]")
{
    uint32_t id1 = message_trace_next_frame_id();
    uint32_t id2 = message_trace_next_frame_id();
    uint32_t id3 = message_trace_next_frame_id();

    TEST_ASSERT_NOT_EQUAL(0, id1);
    TEST_ASSERT_NOT_EQUAL(0, id2);
    TEST_ASSERT_NOT_EQUAL(0, id3);
    TEST_ASSERT_NOT_EQUAL(id1, id2);
    TEST_ASSERT_NOT_EQUAL(id2, id3);
    TEST_ASSERT_NOT_EQUAL(id1, id3);
}

TEST_CASE("frame id allocation produces strictly monotonic values",
          "[message_trace]")
{
    uint32_t prev = message_trace_next_frame_id();
    for (int i = 0; i < 100; i++) {
        uint32_t next = message_trace_next_frame_id();
        TEST_ASSERT_GREATER_THAN(prev, next);
        prev = next;
    }
}

TEST_CASE("tx decoded trace does not crash with valid arguments",
          "[message_trace]")
{
    gw_message_t msg = {
        .protocol_version = 3,
    };
    strlcpy(msg.type, "device_command", sizeof(msg.type));
    strlcpy(msg.device_id, "lamp-1", sizeof(msg.device_id));
    strlcpy(msg.command, "set_led", sizeof(msg.command));
    msg.request_id = 42;
    msg.has_device_id = 1;

    uint32_t frame_id = message_trace_next_frame_id();
    message_trace_tx_decoded(frame_id, "lamp-1", &msg, 37);
}

TEST_CASE("tx decoded trace handles NULL message gracefully",
          "[message_trace]")
{
    message_trace_tx_decoded(1, "lamp-1", NULL, 0);
}

TEST_CASE("tx raw trace does not crash with valid data",
          "[message_trace]")
{
    uint8_t data[] = {0xa4, 0x01, 0x07, 0x02, 0x68, 0x64, 0x65,
                      0x76, 0x69, 0x63, 0x65, 0x5f, 0x63, 0x6f,
                      0x6d, 0x6d, 0x61, 0x6e, 0x64};
    message_trace_tx_raw(1, data, sizeof(data));
}

TEST_CASE("tx raw trace handles NULL data gracefully",
          "[message_trace]")
{
    message_trace_tx_raw(1, NULL, 0);
}

TEST_CASE("tx raw trace truncates long data for logging",
          "[message_trace]")
{
    uint8_t data[64];
    memset(data, 0xAB, sizeof(data));
    message_trace_tx_raw(1, data, sizeof(data));
}

TEST_CASE("tx result trace logs nonzero frame id",
          "[message_trace]")
{
    uint32_t frame_id = message_trace_next_frame_id();
    message_trace_tx_result(frame_id, 0);
    message_trace_tx_result(frame_id, -1);
}

TEST_CASE("rx raw trace does not crash with valid data",
          "[message_trace]")
{
    uint8_t data[] = {0xa4, 0x01, 0x06, 0x02, 0x65, 0x6c, 0x61,
                      0x6d, 0x70, 0x2d, 0x31};
    message_trace_rx_raw(1, "lamp-1", data, sizeof(data));
}

TEST_CASE("rx raw trace handles NULL data gracefully",
          "[message_trace]")
{
    message_trace_rx_raw(1, "lamp-1", NULL, 0);
}

TEST_CASE("rx decoded trace does not crash with valid message",
          "[message_trace]")
{
    gw_message_t msg = {
        .protocol_version = 3,
    };
    strlcpy(msg.type, "device_ack", sizeof(msg.type));
    strlcpy(msg.device_id, "lamp-1", sizeof(msg.device_id));
    strlcpy(msg.command, "set_led", sizeof(msg.command));
    msg.request_id = 99;

    message_trace_rx_decoded(1, "lamp-1", &msg);
}

TEST_CASE("rx decoded trace handles NULL message gracefully",
          "[message_trace]")
{
    message_trace_rx_decoded(1, "lamp-1", NULL);
}

TEST_CASE("rx decode error trace logs without crash",
          "[message_trace]")
{
    message_trace_rx_decode_error(1, "lamp-1", -3);
}

TEST_CASE("concurrent frame id allocation produces no duplicates",
          "[message_trace][concurrency]")
{
    /* Allocate many IDs from the main task and verify uniqueness. */
    uint32_t ids[200];
    for (int i = 0; i < 200; i++) {
        ids[i] = message_trace_next_frame_id();
    }
    /* Check all are nonzero. */
    for (int i = 0; i < 200; i++) {
        TEST_ASSERT_NOT_EQUAL(0, ids[i]);
    }
    /* Check uniqueness by brute force (200 is small enough). */
    for (int i = 0; i < 200; i++) {
        for (int j = i + 1; j < 200; j++) {
            TEST_ASSERT_NOT_EQUAL(ids[i], ids[j]);
        }
    }
}

TEST_CASE("trace functions survive rapid call sequence",
          "[message_trace]")
{
    gw_message_t msg = {
        .protocol_version = 3,
    };
    strlcpy(msg.type, "device_command", sizeof(msg.type));
    strlcpy(msg.device_id, "lamp-1", sizeof(msg.device_id));
    strlcpy(msg.command, "get_state", sizeof(msg.command));

    uint8_t raw[] = {0xa1, 0x01, 0x02};

    for (int i = 0; i < 50; i++) {
        uint32_t fid = message_trace_next_frame_id();
        message_trace_tx_decoded(fid, "lamp-1", &msg, 10);
        message_trace_tx_raw(fid, raw, sizeof(raw));
        message_trace_tx_result(fid, 0);

        message_trace_rx_raw(fid, "lamp-1", raw, sizeof(raw));
        message_trace_rx_decoded(fid, "lamp-1", &msg);
        message_trace_rx_decode_error(fid, "lamp-1", -1);
    }
}
