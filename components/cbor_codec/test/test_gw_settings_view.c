#include <string.h>

#include "unity.h"
#include "gw_settings_view.h"
#include "qcbor/qcbor_encode.h"

static size_t build_value_frame(uint8_t *buf, size_t cap, uint8_t type,
                                bool value_before_type)
{
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){buf, cap});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_ID, "setting_a");
    if (value_before_type) {
        if (type == GW_SETTING_TYPE_BOOL) {
            QCBOREncode_AddBoolToMapN(&ctx, GW_KEY_SETTINGS_VALUE, true);
        } else if (type == GW_SETTING_TYPE_INT) {
            QCBOREncode_AddInt64ToMapN(&ctx, GW_KEY_SETTINGS_VALUE, -7);
        } else if (type == GW_SETTING_TYPE_STRING) {
            QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_VALUE, "hello");
        } else {
            QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_VALUE, 3);
        }
    }
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_TYPE, type);
    if (!value_before_type) {
        if (type == GW_SETTING_TYPE_BOOL) {
            QCBOREncode_AddBoolToMapN(&ctx, GW_KEY_SETTINGS_VALUE, true);
        } else if (type == GW_SETTING_TYPE_INT) {
            QCBOREncode_AddInt64ToMapN(&ctx, GW_KEY_SETTINGS_VALUE, -7);
        } else if (type == GW_SETTING_TYPE_STRING) {
            QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_VALUE, "hello");
        } else {
            QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_VALUE, 3);
        }
    }
    QCBOREncode_CloseMap(&ctx);
    UsefulBufC encoded;
    if (QCBOREncode_Finish(&ctx, &encoded) != QCBOR_SUCCESS) return 0;
    return encoded.len;
}

TEST_CASE("DS-CBOR-004: parses full INT schema item", "[cbor_codec][g1]")
{
    uint8_t frame[256];
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){frame, sizeof(frame)});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_SEQUENCE, 2);
    QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_ID, "brightness");
    QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_TITLE, "Brightness");
    QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_GROUP, "display");
    QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_UNIT, "%");
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_TYPE, GW_SETTING_TYPE_INT);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_FLAGS, 0);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_MAX_LENGTH, 0);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_OPTION_COUNT, 0);
    QCBOREncode_AddInt64ToMapN(&ctx, GW_KEY_SETTINGS_VALUE, 42);
    QCBOREncode_CloseMap(&ctx);
    UsefulBufC encoded;
    TEST_ASSERT_EQUAL(QCBOR_SUCCESS, QCBOREncode_Finish(&ctx, &encoded));

    gw_settings_snapshot_t snapshot;
    gw_settings_frame_view_t view = {.buf = encoded.ptr, .len = encoded.len};
    TEST_ASSERT_EQUAL_INT(0, gw_settings_frame_view_parse(&view, encoded.len, &snapshot));
    TEST_ASSERT_TRUE(snapshot.view.has_sequence);
    TEST_ASSERT_EQUAL_UINT16(2, snapshot.view.sequence);
    TEST_ASSERT_EQUAL_STRING("brightness", snapshot.view.setting_id);
    TEST_ASSERT_EQUAL_STRING("Brightness", snapshot.view.title);
    TEST_ASSERT_EQUAL_STRING("display", snapshot.view.group);
    TEST_ASSERT_EQUAL_STRING("%", snapshot.view.unit);
    TEST_ASSERT_EQUAL_UINT8(GW_SETTING_TYPE_INT, snapshot.entries[0].setting_type);
    TEST_ASSERT_EQUAL_INT32(42, snapshot.entries[0].int_val);
}

TEST_CASE("DS-CBOR-005: decodes readonly flags", "[cbor_codec][g1]")
{
    uint8_t frame[128];
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){frame, sizeof(frame)});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_ID, "locked");
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_TYPE, GW_SETTING_TYPE_BOOL);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_FLAGS, GW_SETTING_FLAG_READONLY);
    QCBOREncode_CloseMap(&ctx);
    UsefulBufC encoded;
    TEST_ASSERT_EQUAL(QCBOR_SUCCESS, QCBOREncode_Finish(&ctx, &encoded));

    gw_settings_snapshot_t snapshot;
    gw_settings_frame_view_t view = {.buf = encoded.ptr, .len = encoded.len};
    TEST_ASSERT_EQUAL_INT(0, gw_settings_frame_view_parse(&view, encoded.len, &snapshot));
    TEST_ASSERT_TRUE(snapshot.entries[0].flags & GW_SETTING_FLAG_READONLY);
    TEST_ASSERT_FALSE(snapshot.entries[0].writable);
}

TEST_CASE("DS-CBOR-006: decodes enum option metadata", "[cbor_codec][g1]")
{
    uint8_t frame[128];
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){frame, sizeof(frame)});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_ID, "mode");
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_TYPE, GW_SETTING_TYPE_ENUM);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_OPTION_INDEX, 1);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_OPTION_COUNT, 3);
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_VALUE, 1);
    QCBOREncode_CloseMap(&ctx);
    UsefulBufC encoded;
    TEST_ASSERT_EQUAL(QCBOR_SUCCESS, QCBOREncode_Finish(&ctx, &encoded));

    gw_settings_snapshot_t snapshot;
    gw_settings_frame_view_t view = {.buf = encoded.ptr, .len = encoded.len};
    TEST_ASSERT_EQUAL_INT(0, gw_settings_frame_view_parse(&view, encoded.len, &snapshot));
    TEST_ASSERT_TRUE(snapshot.view.has_option_index);
    TEST_ASSERT_EQUAL_UINT8(1, snapshot.view.option_index);
    TEST_ASSERT_EQUAL_UINT16(3, snapshot.view.option_count);
    TEST_ASSERT_EQUAL_INT32(1, snapshot.entries[0].enum_val.value);
}

TEST_CASE("DS-CBOR-007: decodes BOOL key40", "[cbor_codec][g1]")
{
    uint8_t frame[128];
    size_t len = build_value_frame(frame, sizeof(frame), GW_SETTING_TYPE_BOOL, false);
    gw_settings_value_entry_t entry;
    gw_settings_frame_view_t view = {.buf = frame, .len = len};
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_INT(0, gw_settings_value_frame_view_parse(&view, len, &entry));
    TEST_ASSERT_TRUE(entry.bool_val);
}

TEST_CASE("DS-CBOR-008: decodes INT key40", "[cbor_codec][g1]")
{
    uint8_t frame[128];
    size_t len = build_value_frame(frame, sizeof(frame), GW_SETTING_TYPE_INT, false);
    gw_settings_value_entry_t entry;
    gw_settings_frame_view_t view = {.buf = frame, .len = len};
    TEST_ASSERT_EQUAL_INT(0, gw_settings_value_frame_view_parse(&view, len, &entry));
    TEST_ASSERT_EQUAL_INT32(-7, entry.int_val);
}

TEST_CASE("DS-CBOR-009: decodes STRING key40", "[cbor_codec][g1]")
{
    uint8_t frame[128];
    size_t len = build_value_frame(frame, sizeof(frame), GW_SETTING_TYPE_STRING, false);
    gw_settings_value_entry_t entry;
    gw_settings_frame_view_t view = {.buf = frame, .len = len};
    TEST_ASSERT_EQUAL_INT(0, gw_settings_value_frame_view_parse(&view, len, &entry));
    TEST_ASSERT_EQUAL_STRING("hello", entry.string_val.str);
}

TEST_CASE("DS-CBOR-010: decodes ENUM key40", "[cbor_codec][g1]")
{
    uint8_t frame[128];
    size_t len = build_value_frame(frame, sizeof(frame), GW_SETTING_TYPE_ENUM, false);
    gw_settings_value_entry_t entry;
    gw_settings_frame_view_t view = {.buf = frame, .len = len};
    TEST_ASSERT_EQUAL_INT(0, gw_settings_value_frame_view_parse(&view, len, &entry));
    TEST_ASSERT_EQUAL_INT32(3, entry.enum_val);
}

TEST_CASE("DS-CBOR-011: decodes value before type", "[cbor_codec][g1]")
{
    uint8_t frame[128];
    size_t len = build_value_frame(frame, sizeof(frame), GW_SETTING_TYPE_INT, true);
    gw_settings_value_entry_t entry;
    gw_settings_frame_view_t view = {.buf = frame, .len = len};
    TEST_ASSERT_EQUAL_INT(0, gw_settings_value_frame_view_parse(&view, len, &entry));
    TEST_ASSERT_EQUAL_INT32(-7, entry.int_val);
}

TEST_CASE("DS-CBOR-012: rejects oversized string", "[cbor_codec][g1]")
{
    char oversized[GW_SETTINGS_VALUE_MAX_LEN + 1];
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    uint8_t frame[256];
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){frame, sizeof(frame)});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_ID, "text");
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_TYPE, GW_SETTING_TYPE_STRING);
    QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_VALUE, oversized);
    QCBOREncode_CloseMap(&ctx);
    UsefulBufC encoded;
    TEST_ASSERT_EQUAL(QCBOR_SUCCESS, QCBOREncode_Finish(&ctx, &encoded));
    gw_settings_value_entry_t entry;
    gw_settings_frame_view_t view = {.buf = encoded.ptr, .len = encoded.len};
    TEST_ASSERT_EQUAL_INT(-1, gw_settings_value_frame_view_parse(&view, encoded.len, &entry));
}

TEST_CASE("DS-CBOR-013: tolerates unknown key", "[cbor_codec][g1]")
{
    uint8_t frame[128];
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){frame, sizeof(frame)});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapN(&ctx, GW_KEY_SETTINGS_ID, "known");
    QCBOREncode_AddUInt64ToMapN(&ctx, GW_KEY_SETTINGS_TYPE, GW_SETTING_TYPE_BOOL);
    QCBOREncode_AddUInt64ToMapN(&ctx, 99, 7);
    QCBOREncode_CloseMap(&ctx);
    UsefulBufC encoded;
    TEST_ASSERT_EQUAL(QCBOR_SUCCESS, QCBOREncode_Finish(&ctx, &encoded));
    gw_settings_snapshot_t snapshot;
    gw_settings_frame_view_t view = {.buf = encoded.ptr, .len = encoded.len};
    TEST_ASSERT_EQUAL_INT(0, gw_settings_frame_view_parse(&view, encoded.len, &snapshot));
}

TEST_CASE("DS-CBOR-014: rejects trailing bytes", "[cbor_codec][g1]")
{
    uint8_t frame[128];
    size_t len = build_value_frame(frame, sizeof(frame), GW_SETTING_TYPE_BOOL, false);
    frame[len++] = 0;
    gw_settings_value_entry_t entry;
    gw_settings_frame_view_t view = {.buf = frame, .len = len};
    TEST_ASSERT_EQUAL_INT(-1, gw_settings_value_frame_view_parse(&view, len, &entry));
}

TEST_CASE("Settings request encoder produces canonical read command", "[cbor_codec][g1]")
{
    uint8_t out[GW_MSG_MAX_LEN];
    int len = gw_settings_encode_request(GW_SETTINGS_CMD_READ_SETTINGS, "lamp-1", out, sizeof(out));
    gw_message_t msg;
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_INT(0, cbor_codec_decode(out, len, &msg));
    TEST_ASSERT_EQUAL_STRING(GW_SETTINGS_CMD_READ_SETTINGS, msg.command);
}
