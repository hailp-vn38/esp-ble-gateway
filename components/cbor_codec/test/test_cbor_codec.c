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
        .protocol_version = GW_PROTOCOL_VERSION,
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

TEST_CASE("CBOR encode/decode preserves request_id", "[cbor_codec]")
{
    const gw_message_t input = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .type = "device_command",
        .device_id = "relay-1",
        .command = "set_power",
        .request_id = 1042,
        .has_request_id = 1,
        .has_device_id = 1,
    };
    uint8_t encoded[GW_MSG_MAX_LEN];
    int encoded_length = cbor_codec_encode(&input, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, encoded_length);

    gw_message_t decoded;
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_decode(encoded, encoded_length, &decoded));
    TEST_ASSERT_TRUE(decoded.has_request_id);
    TEST_ASSERT_EQUAL_UINT32(1042, decoded.request_id);

    // Messages without request_id decode with the flag cleared.
    gw_message_t plain = input;
    plain.request_id = 0;
    plain.has_request_id = 0;
    encoded_length = cbor_codec_encode(&plain, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, encoded_length);
    TEST_ASSERT_EQUAL_INT(0,
                          cbor_codec_decode(encoded, encoded_length, &decoded));
    TEST_ASSERT_FALSE(decoded.has_request_id);
}

TEST_CASE("CBOR v4 preserves capability item metadata", "[cbor_codec]")
{
    const gw_message_t input = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .type = "capability_item",
        .device_id = "lamp-1",
        .command = "set_brightness",
        .has_device_id = 1,
        .snapshot_id = 88,
        .has_snapshot_id = 1,
        .sequence = 1,
        .has_sequence = 1,
        .value_type = 2,
        .has_value_type = 1,
        .capability_flags = 1,
        .has_capability_flags = 1,
        .min_value = 0,
        .has_min_value = 1,
        .max_value = 100,
        .has_max_value = 1,
        .step = 5,
        .has_step = 1,
        .capability_label = "Brightness",
        .capability_unit = "%",
    };
    uint8_t encoded[GW_MSG_MAX_LEN];
    int length = cbor_codec_encode(&input, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, length);

    gw_message_t decoded;
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_decode(encoded, length, &decoded));
    TEST_ASSERT_EQUAL_UINT32(88, decoded.snapshot_id);
    TEST_ASSERT_EQUAL_UINT16(1, decoded.sequence);
    TEST_ASSERT_EQUAL_UINT8(2, decoded.value_type);
    TEST_ASSERT_EQUAL_INT32(0, decoded.min_value);
    TEST_ASSERT_EQUAL_INT32(100, decoded.max_value);
    TEST_ASSERT_EQUAL_UINT32(5, decoded.step);
    TEST_ASSERT_EQUAL_STRING("Brightness", decoded.capability_label);
    TEST_ASSERT_EQUAL_STRING("%", decoded.capability_unit);
    TEST_ASSERT_TRUE(decoded.has_snapshot_id);
    TEST_ASSERT_TRUE(decoded.has_sequence);
    TEST_ASSERT_TRUE(decoded.has_value_type);
}

TEST_CASE("CBOR v4 preserves semantic feature fields", "[cbor_codec]")
{
    const gw_message_t input = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .type = "feature_item",
        .device_id = "lamp-1",
        .command = "describe_capabilities",
        .has_device_id = 1,
        .snapshot_id = 88,
        .has_snapshot_id = 1,
        .sequence = 0,
        .has_sequence = 1,
        .feature_id = "led_main",
        .has_feature_id = 1,
        .feature_type = GW_FEATURE_ON_OFF_LIGHT,
        .has_feature_type = 1,
        .feature_schema_version = 1,
        .has_feature_schema_version = 1,
        .feature_flags = 0x0003,
        .has_feature_flags = 1,
        .property_id = GW_PROP_ON_OFF,
        .has_property_id = 1,
        .feature_tool = "set_led",
        .has_feature_tool = 1,
        .value_type = 1,
        .has_value_type = 1,
    };
    uint8_t encoded[GW_MSG_MAX_LEN];
    int length = cbor_codec_encode(&input, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, length);

    gw_message_t decoded;
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_decode(encoded, length, &decoded));
    TEST_ASSERT_TRUE(decoded.has_feature_id);
    TEST_ASSERT_EQUAL_STRING("led_main", decoded.feature_id);
    TEST_ASSERT_TRUE(decoded.has_feature_type);
    TEST_ASSERT_EQUAL_UINT8(GW_FEATURE_ON_OFF_LIGHT, decoded.feature_type);
    TEST_ASSERT_TRUE(decoded.has_feature_schema_version);
    TEST_ASSERT_EQUAL_UINT16(1, decoded.feature_schema_version);
    TEST_ASSERT_TRUE(decoded.has_feature_flags);
    TEST_ASSERT_EQUAL_UINT16(0x0003, decoded.feature_flags);
    TEST_ASSERT_TRUE(decoded.has_property_id);
    TEST_ASSERT_EQUAL_UINT8(GW_PROP_ON_OFF, decoded.property_id);
    TEST_ASSERT_TRUE(decoded.has_feature_tool);
    TEST_ASSERT_EQUAL_STRING("set_led", decoded.feature_tool);

    // feature_state event with a boolean value.
    const gw_message_t event = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .type = "device_event",
        .device_id = "lamp-1",
        .command = "feature_state",
        .has_device_id = 1,
        .feature_id = "led_main",
        .has_feature_id = 1,
        .property_id = GW_PROP_ON_OFF,
        .has_property_id = 1,
        .feature_value_bool = true,
        .has_feature_value_bool = 1,
        .feature_value_int = -12000,
        .has_feature_value_int = 1,
        .int_value = 0,
        .bool_value = 0,
    };
    length = cbor_codec_encode(&event, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, length);
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_decode(encoded, length, &decoded));
    TEST_ASSERT_TRUE(decoded.has_feature_value_bool);
    TEST_ASSERT_TRUE(decoded.feature_value_bool);
    TEST_ASSERT_TRUE(decoded.has_feature_value_int);
    TEST_ASSERT_EQUAL_INT32(-12000, decoded.feature_value_int);
}

TEST_CASE("CBOR decoder ignores reserved key 7", "[cbor_codec]")
{
    // {0:4, 1:"t", 3:"c", 4:0, 5:false, 7:"legacy"} — key 7 carries a
    // former device_type value and must be skipped, not rejected.
    static const uint8_t WITH_RESERVED_KEY[] = {
        0xA6, 0x00, 0x04, 0x01, 0x61, 't',  0x03, 0x61, 'c',
        0x04, 0x00, 0x05, 0xF4, 0x07, 0x63, 'o', 'l', 'd',
    };
    gw_message_t decoded;
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_decode(WITH_RESERVED_KEY,
                                               sizeof(WITH_RESERVED_KEY),
                                               &decoded));
    TEST_ASSERT_EQUAL_UINT8(GW_PROTOCOL_VERSION, decoded.protocol_version);
}

TEST_CASE("JSON conversion validates and preserves values", "[cbor_codec]")
{
    const char *json =
        "{\"protocol_version\":4,\"type\":\"gateway_command\","
        "\"device_id\":\"sensor-1\",\"command\":\"add_device\","
        "\"int_value\":12,\"bool_value\":true,\"name\":\"Sensor\","
        "\"ble_addr\":\"11:22:33:44:55:66\","
        "\"ble_addr_type\":1}";
    gw_message_t message;
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_json_to_msg(json, &message));
    TEST_ASSERT_EQUAL_STRING("sensor-1", message.device_id);
    static const uint8_t expected_addr[] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_addr, message.ble_addr, 6);

    char output[512];
    TEST_ASSERT_GREATER_THAN(0,
                             cbor_codec_msg_to_json(&message, output, sizeof(output)));
    TEST_ASSERT_NULL(strstr(output, "device_type"));
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

    // Strict v4: the encoder rejects anything but the current version.
    message.protocol_version = 0;
    TEST_ASSERT_EQUAL_INT(-1, cbor_codec_encode(&message, output, sizeof(output)));
    message.protocol_version = GW_PROTOCOL_VERSION - 1;
    TEST_ASSERT_EQUAL_INT(-1, cbor_codec_encode(&message, output, sizeof(output)));
}

TEST_CASE("CBOR decoder requires explicit protocol version 4", "[cbor_codec]")
{
    // Minimal map: {0:version, 1:"t", 3:"c", 4:0, 5:false}.
    static const uint8_t V1[] = {0xA5, 0x00, 0x01, 0x01, 0x61, 't',
                                 0x03, 0x61, 'c', 0x04, 0x00, 0x05, 0xF4};
    static const uint8_t V2[] = {0xA5, 0x00, 0x02, 0x01, 0x61, 't',
                                 0x03, 0x61, 'c', 0x04, 0x00, 0x05, 0xF4};
    static const uint8_t V3[] = {0xA5, 0x00, 0x03, 0x01, 0x61, 't',
                                 0x03, 0x61, 'c', 0x04, 0x00, 0x05, 0xF4};
    static const uint8_t V4[] = {0xA5, 0x00, 0x04, 0x01, 0x61, 't',
                                 0x03, 0x61, 'c', 0x04, 0x00, 0x05, 0xF4};
    static const uint8_t V5[] = {0xA5, 0x00, 0x05, 0x01, 0x61, 't',
                                 0x03, 0x61, 'c', 0x04, 0x00, 0x05, 0xF4};
    static const uint8_t VERSION_ABSENT[] = {0xA4, 0x01, 0x61, 't', 0x03,
                                             0x61, 'c', 0x04, 0x00, 0x05,
                                             0xF4};
    gw_message_t message;
    TEST_ASSERT_EQUAL_INT(-1, cbor_codec_decode(V1, sizeof(V1), &message));
    TEST_ASSERT_EQUAL_INT(-1, cbor_codec_decode(V2, sizeof(V2), &message));
    TEST_ASSERT_EQUAL_INT(-1, cbor_codec_decode(V3, sizeof(V3), &message));
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_decode(V4, sizeof(V4), &message));
    TEST_ASSERT_EQUAL_INT(-1, cbor_codec_decode(V5, sizeof(V5), &message));
    TEST_ASSERT_EQUAL_INT(-1, cbor_codec_decode(VERSION_ABSENT,
                                                sizeof(VERSION_ABSENT),
                                                &message));

    // JSON path is strict too: v3 is rejected, v4 accepted.
    TEST_ASSERT_EQUAL_INT(-1, cbor_codec_json_to_msg(
                                  "{\"protocol_version\":3,\"type\":\"t\","
                                  "\"command\":\"c\",\"int_value\":0,"
                                  "\"bool_value\":false}", &message));
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_json_to_msg(
                                 "{\"protocol_version\":4,\"type\":\"t\","
                                 "\"command\":\"c\",\"int_value\":0,"
                                 "\"bool_value\":false}", &message));
}
