#include <string.h>

#include "unity.h"

#include "gw_settings_view.h"
#include "qcbor/qcbor_encode.h"

/* ── Helper: build a settings_item CBOR frame using QCBOR ──────────── */

static size_t build_bool_setting_frame(uint8_t *buf, size_t cap,
                                       const char *setting_id,
                                       bool writable, bool default_val)
{
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){buf, cap});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapN(&ctx,
                                  GW_SETTINGS_CBOR_KEY_SETTING_ID, setting_id);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_SETTING_TYPE,
                                GW_SETTINGS_TYPE_BOOL);
    QCBOREncode_AddBoolToMapN(&ctx, GW_SETTINGS_CBOR_KEY_WRITABLE, writable);
    QCBOREncode_AddInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_DEFAULT_VALUE,
                               default_val ? 1 : 0);
    QCBOREncode_CloseMap(&ctx);

    UsefulBufC encoded;
    if (QCBOREncode_Finish(&ctx, &encoded) != QCBOR_SUCCESS) return 0;
    return encoded.len;
}

static size_t build_int_setting_frame(uint8_t *buf, size_t cap,
                                      const char *setting_id,
                                      bool writable, int32_t default_val,
                                      int32_t min_val, int32_t max_val,
                                      uint32_t step)
{
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){buf, cap});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapN(&ctx,
                                  GW_SETTINGS_CBOR_KEY_SETTING_ID, setting_id);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_SETTING_TYPE,
                                GW_SETTINGS_TYPE_INT);
    QCBOREncode_AddBoolToMapN(&ctx, GW_SETTINGS_CBOR_KEY_WRITABLE, writable);
    QCBOREncode_AddInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_DEFAULT_VALUE,
                               default_val);
    QCBOREncode_AddInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_MIN_VALUE, min_val);
    QCBOREncode_AddInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_MAX_VALUE, max_val);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_STEP, step);
    QCBOREncode_CloseMap(&ctx);

    UsefulBufC encoded;
    if (QCBOREncode_Finish(&ctx, &encoded) != QCBOR_SUCCESS) return 0;
    return encoded.len;
}

static size_t build_enum_setting_frame(uint8_t *buf, size_t cap,
                                       const char *setting_id,
                                       bool writable, int32_t default_val,
                                       int num_options)
{
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){buf, cap});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapN(&ctx,
                                  GW_SETTINGS_CBOR_KEY_SETTING_ID, setting_id);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_SETTING_TYPE,
                                GW_SETTINGS_TYPE_ENUM);
    QCBOREncode_AddBoolToMapN(&ctx, GW_SETTINGS_CBOR_KEY_WRITABLE, writable);
    QCBOREncode_AddInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_DEFAULT_VALUE,
                               default_val);

    QCBOREncode_OpenArrayInMapN(&ctx, GW_SETTINGS_CBOR_KEY_ENUM_OPTIONS);
    for (int i = 0; i < num_options; i++) {
        QCBOREncode_OpenMap(&ctx);
        QCBOREncode_AddInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_ENUM_VALUE, i);
        char label[16];
        snprintf(label, sizeof(label), "opt_%d", i);
        QCBOREncode_AddSZStringToMapN(&ctx, GW_SETTINGS_CBOR_KEY_ENUM_LABEL,
                                      label);
        QCBOREncode_CloseMap(&ctx);
    }
    QCBOREncode_CloseArray(&ctx);

    QCBOREncode_CloseMap(&ctx);

    UsefulBufC encoded;
    if (QCBOREncode_Finish(&ctx, &encoded) != QCBOR_SUCCESS) return 0;
    return encoded.len;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

TEST_CASE("Settings view parses bool setting", "[settings_view][g0]")
{
    uint8_t frame[128];
    size_t len = build_bool_setting_frame(frame, sizeof(frame),
                                          "night_mode", true, false);
    TEST_ASSERT_GREATER_THAN(0, len);

    gw_settings_frame_view_t view = { .buf = frame, .len = len };
    gw_settings_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(0, gw_settings_frame_view_parse(&view, len, &snap));
    TEST_ASSERT_EQUAL_UINT16(1, snap.settings_count);
    TEST_ASSERT_EQUAL_STRING("night_mode", snap.entries[0].setting_id);
    TEST_ASSERT_EQUAL_UINT8(GW_SETTINGS_TYPE_BOOL, snap.entries[0].setting_type);
    TEST_ASSERT_TRUE(snap.entries[0].writable);
    TEST_ASSERT_TRUE(snap.entries[0].has_value);
    TEST_ASSERT_FALSE(snap.entries[0].bool_val);
}

TEST_CASE("Settings view parses int setting with range", "[settings_view][g0]")
{
    uint8_t frame[128];
    size_t len = build_int_setting_frame(frame, sizeof(frame),
                                         "brightness", true, 50, 0, 100, 5);
    TEST_ASSERT_GREATER_THAN(0, len);

    gw_settings_frame_view_t view = { .buf = frame, .len = len };
    gw_settings_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(0, gw_settings_frame_view_parse(&view, len, &snap));
    TEST_ASSERT_EQUAL_STRING("brightness", snap.entries[0].setting_id);
    TEST_ASSERT_EQUAL_UINT8(GW_SETTINGS_TYPE_INT, snap.entries[0].setting_type);
    TEST_ASSERT_TRUE(snap.entries[0].writable);
    TEST_ASSERT_TRUE(snap.entries[0].has_value);
    TEST_ASSERT_EQUAL_INT32(50, snap.entries[0].int_val);
    TEST_ASSERT_EQUAL_INT32(0, snap.entries[0].numeric_range.min_value);
    TEST_ASSERT_EQUAL_INT32(100, snap.entries[0].numeric_range.max_value);
    TEST_ASSERT_EQUAL_UINT32(5, snap.entries[0].numeric_range.step);
}

TEST_CASE("Settings view parses enum setting", "[settings_view][g0]")
{
    uint8_t frame[256];
    size_t len = build_enum_setting_frame(frame, sizeof(frame),
                                          "fan_speed", true, 1, 3);
    TEST_ASSERT_GREATER_THAN(0, len);

    gw_settings_frame_view_t view = { .buf = frame, .len = len };
    gw_settings_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(0, gw_settings_frame_view_parse(&view, len, &snap));
    TEST_ASSERT_EQUAL_STRING("fan_speed", snap.entries[0].setting_id);
    TEST_ASSERT_EQUAL_UINT8(GW_SETTINGS_TYPE_ENUM, snap.entries[0].setting_type);
    TEST_ASSERT_TRUE(snap.entries[0].writable);
    TEST_ASSERT_EQUAL_INT32(1, snap.entries[0].enum_val.value);
    TEST_ASSERT_EQUAL_UINT8(3, snap.entries[0].enum_option_count);
    TEST_ASSERT_EQUAL_INT32(0, snap.entries[0].enum_options[0].value);
    TEST_ASSERT_EQUAL_STRING("opt_0", snap.entries[0].enum_options[0].label);
    TEST_ASSERT_EQUAL_INT32(2, snap.entries[0].enum_options[2].value);
    TEST_ASSERT_EQUAL_STRING("opt_2", snap.entries[0].enum_options[2].label);
}

TEST_CASE("Settings view rejects null/empty inputs", "[settings_view][g0]")
{
    gw_settings_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(-1, gw_settings_frame_view_parse(NULL, 0, &snap));

    uint8_t frame[4] = {0xff, 0x01, 0x02, 0x03};
    gw_settings_frame_view_t view = { .buf = frame, .len = sizeof(frame) };
    TEST_ASSERT_EQUAL_INT(-1, gw_settings_frame_view_parse(&view,
                                                           sizeof(frame),
                                                           &snap));
}

TEST_CASE("Settings view rejects truncated CBOR", "[settings_view][g0]")
{
    /* Build a valid frame, then truncate it. */
    uint8_t frame[128];
    size_t len = build_bool_setting_frame(frame, sizeof(frame),
                                          "night_mode", true, false);
    TEST_ASSERT_GREATER_THAN(0, len);

    /* Truncate to half. */
    gw_settings_frame_view_t view = { .buf = frame, .len = len / 2 };
    gw_settings_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(-1, gw_settings_frame_view_parse(&view, len / 2,
                                                           &snap));
}

TEST_CASE("Settings view tolerates unknown keys", "[settings_view][g0]")
{
    /* Build a frame with the known keys plus an unknown key 99. */
    uint8_t frame[128];
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){frame, sizeof(frame)});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapN(&ctx, GW_SETTINGS_CBOR_KEY_SETTING_ID,
                                  "test_setting");
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_SETTING_TYPE,
                                GW_SETTINGS_TYPE_BOOL);
    QCBOREncode_AddBoolToMapN(&ctx, GW_SETTINGS_CBOR_KEY_WRITABLE, false);
    /* Unknown key 99. */
    QCBOREncode_AddUInt64ToMapN(&ctx, 99, 42);
    QCBOREncode_CloseMap(&ctx);

    UsefulBufC encoded;
    TEST_ASSERT_EQUAL(QCBOR_SUCCESS, QCBOREncode_Finish(&ctx, &encoded));

    gw_settings_frame_view_t view = { .buf = encoded.ptr, .len = encoded.len };
    gw_settings_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(0, gw_settings_frame_view_parse(&view, encoded.len,
                                                          &snap));
    TEST_ASSERT_EQUAL_STRING("test_setting", snap.entries[0].setting_id);
}

TEST_CASE("Settings request encoder produces valid CBOR",
          "[settings_view][g0]")
{
    uint8_t out[GW_MSG_MAX_LEN];
    int len = gw_settings_encode_request(
        GW_SETTINGS_CMD_DESCRIBE_SETTINGS, "lamp-1", out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, len);

    /* Decode back and verify. */
    gw_message_t msg;
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_decode(out, len, &msg));
    TEST_ASSERT_EQUAL_UINT8(GW_PROTOCOL_VERSION, msg.protocol_version);
    TEST_ASSERT_EQUAL_STRING("describe_settings", msg.type);
    TEST_ASSERT_EQUAL_STRING("lamp-1", msg.device_id);
    TEST_ASSERT_EQUAL_STRING("describe_settings", msg.command);
}

TEST_CASE("Settings request encoder rejects invalid inputs",
          "[settings_view][g0]")
{
    uint8_t out[GW_MSG_MAX_LEN];
    TEST_ASSERT_EQUAL_INT(-1, gw_settings_encode_request(NULL, "lamp-1",
                                                         out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, gw_settings_encode_request("cmd", NULL,
                                                         out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, gw_settings_encode_request("cmd", "lamp-1",
                                                         NULL, 0));
}

TEST_CASE("Settings view with config_revision", "[settings_view][g0]")
{
    uint8_t frame[128];
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){frame, sizeof(frame)});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapN(&ctx, GW_SETTINGS_CBOR_KEY_SETTING_ID,
                                  "test_rev");
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_SETTING_TYPE,
                                GW_SETTINGS_TYPE_BOOL);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_CONFIG_REVISION, 7);
    QCBOREncode_CloseMap(&ctx);

    UsefulBufC encoded;
    TEST_ASSERT_EQUAL(QCBOR_SUCCESS, QCBOREncode_Finish(&ctx, &encoded));

    gw_settings_frame_view_t view = { .buf = encoded.ptr, .len = encoded.len };
    gw_settings_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(0, gw_settings_frame_view_parse(&view, encoded.len,
                                                          &snap));
    TEST_ASSERT_EQUAL_UINT32(7, snap.config_revision);
    TEST_ASSERT_EQUAL_STRING("test_rev", snap.entries[0].setting_id);
}

TEST_CASE("Settings view with group metadata", "[settings_view][g0]")
{
    uint8_t frame[128];
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){frame, sizeof(frame)});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapN(&ctx, GW_SETTINGS_CBOR_KEY_SETTING_ID,
                                  "grouped_setting");
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_SETTING_TYPE,
                                GW_SETTINGS_TYPE_INT);
    QCBOREncode_AddSZStringToMapN(&ctx, GW_SETTINGS_CBOR_KEY_GROUP,
                                  "display");
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_SETTINGS_CBOR_KEY_GROUP_ORDER, 2);
    QCBOREncode_CloseMap(&ctx);

    UsefulBufC encoded;
    TEST_ASSERT_EQUAL(QCBOR_SUCCESS, QCBOREncode_Finish(&ctx, &encoded));

    gw_settings_frame_view_t view = { .buf = encoded.ptr, .len = encoded.len };
    gw_settings_snapshot_t snap;
    TEST_ASSERT_EQUAL_INT(0, gw_settings_frame_view_parse(&view, encoded.len,
                                                          &snap));
    TEST_ASSERT_EQUAL_STRING("display", snap.entries[0].group);
    TEST_ASSERT_EQUAL_UINT8(2, snap.entries[0].group_order);
}
