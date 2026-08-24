#include <string.h>

#include "unity.h"

#include "cbor_codec.h"

TEST_CASE("CBOR encode/decode preserves every message field", "[cbor_codec]")
{
    const gw_message_t input = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .type = "device_command",
        .device_id = "lamp-kitchen",
        .command = "set_brightness",
        .int_value = 73,
        .bool_value = 1,
        .has_device_id = 1,
        .name = "Kitchen lamp",
        .device_type = "light",
        .ble_addr = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11},
        .ble_addr_type = 1,
        .has_ble_addr = 1,
    };
    uint8_t encoded[GW_MSG_MAX_LEN];
    int encoded_length = cbor_codec_encode(&input, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, encoded_length);

    gw_message_t decoded;
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_decode(encoded, encoded_length, &decoded));
    TEST_ASSERT_EQUAL_UINT8(GW_PROTOCOL_VERSION, decoded.protocol_version);
    TEST_ASSERT_EQUAL_STRING(input.type, decoded.type);
    TEST_ASSERT_EQUAL_STRING(input.device_id, decoded.device_id);
    TEST_ASSERT_EQUAL_STRING(input.command, decoded.command);
    TEST_ASSERT_EQUAL_INT(input.int_value, decoded.int_value);
    TEST_ASSERT_EQUAL_INT(input.bool_value, decoded.bool_value);
    TEST_ASSERT_EQUAL_STRING(input.name, decoded.name);
    TEST_ASSERT_EQUAL_STRING(input.device_type, decoded.device_type);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(input.ble_addr, decoded.ble_addr, 6);
    TEST_ASSERT_EQUAL_UINT8(input.ble_addr_type, decoded.ble_addr_type);
    TEST_ASSERT_TRUE(decoded.has_device_id);
    TEST_ASSERT_TRUE(decoded.has_ble_addr);
}

TEST_CASE("CBOR omits optional device fields", "[cbor_codec]")
{
    const gw_message_t input = {
        .type = "gateway_command",
        .command = "get_status",
        .int_value = -5,
    };
    uint8_t encoded[GW_MSG_MAX_LEN];
    int encoded_length = cbor_codec_encode(&input, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, encoded_length);

    gw_message_t decoded;
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_decode(encoded, encoded_length, &decoded));
    TEST_ASSERT_FALSE(decoded.has_device_id);
    TEST_ASSERT_FALSE(decoded.has_ble_addr);
    TEST_ASSERT_EQUAL_STRING("", decoded.device_id);
    TEST_ASSERT_EQUAL_INT(-5, decoded.int_value);
}

TEST_CASE("JSON conversion validates and preserves values", "[cbor_codec]")
{
    const char *json =
        "{\"protocol_version\":1,\"type\":\"gateway_command\","
        "\"device_id\":\"sensor-1\",\"command\":\"add_device\","
        "\"int_value\":12,\"bool_value\":true,\"name\":\"Sensor\","
        "\"device_type\":\"sensor\",\"ble_addr\":\"11:22:33:44:55:66\","
        "\"ble_addr_type\":1}";
    gw_message_t message;
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_json_to_msg(json, &message));
    TEST_ASSERT_EQUAL_STRING("sensor-1", message.device_id);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        ((uint8_t[]){0x66, 0x55, 0x44, 0x33, 0x22, 0x11}),
        message.ble_addr, 6);

    char output[512];
    TEST_ASSERT_GREATER_THAN(0,
                             cbor_codec_msg_to_json(&message, output, sizeof(output)));
    gw_message_t reparsed;
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_json_to_msg(output, &reparsed));
    TEST_ASSERT_EQUAL_STRING(message.command, reparsed.command);
    TEST_ASSERT_EQUAL_INT(message.int_value, reparsed.int_value);
    TEST_ASSERT_EQUAL_INT(message.bool_value, reparsed.bool_value);
}

TEST_CASE("CBOR codec rejects malformed data and unsupported versions", "[cbor_codec]")
{
    const uint8_t invalid[] = {0xff, 0x01, 0x02};
    gw_message_t message;
    TEST_ASSERT_EQUAL_INT(-1, cbor_codec_decode(invalid, sizeof(invalid), &message));
    TEST_ASSERT_EQUAL_INT(-1, cbor_codec_json_to_msg(
                                  "{\"type\":\"device_command\"}", &message));

    memset(&message, 0, sizeof(message));
    message.protocol_version = GW_PROTOCOL_VERSION + 1;
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, "get_status", sizeof(message.command));
    uint8_t output[GW_MSG_MAX_LEN];
    TEST_ASSERT_EQUAL_INT(-1, cbor_codec_encode(&message, output, sizeof(output)));
}
