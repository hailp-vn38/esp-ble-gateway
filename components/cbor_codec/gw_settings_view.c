#include <limits.h>
#include <string.h>

#include "qcbor/qcbor_decode.h"
#include "qcbor/qcbor_encode.h"
#include "qcbor/qcbor_spiffy_decode.h"

#include "gw_settings_view.h"

static QCBORError get_optional_text(QCBORDecodeContext *ctx, int64_t key,
                                    UsefulBufC *value)
{
    *value = NULLUsefulBufC;
    QCBORDecode_GetTextStringInMapN(ctx, key, value);
    return QCBORDecode_GetAndResetError(ctx);
}

static QCBORError get_optional_uint(QCBORDecodeContext *ctx, int64_t key,
                                    uint64_t *value)
{
    *value = 0;
    QCBORDecode_GetUInt64InMapN(ctx, key, value);
    return QCBORDecode_GetAndResetError(ctx);
}

static QCBORError get_optional_int(QCBORDecodeContext *ctx, int64_t key,
                                   int64_t *value)
{
    *value = 0;
    QCBORDecode_GetInt64InMapN(ctx, key, value);
    return QCBORDecode_GetAndResetError(ctx);
}

static int copy_text(char *dst, size_t dst_size, UsefulBufC text)
{
    if (dst == NULL || dst_size == 0 || text.ptr == NULL || text.len >= dst_size) {
        return -1;
    }
    if (text.len > 0) memcpy(dst, text.ptr, text.len);
    dst[text.len] = '\0';
    return 0;
}

static int parse_settings_view(const gw_settings_frame_view_t *frame,
                               size_t len, gw_settings_view_t *out)
{
    if (frame == NULL || frame->buf == NULL || len == 0 || out == NULL) return -1;
    memset(out, 0, sizeof(*out));

    QCBORDecodeContext context;
    QCBORDecode_Init(&context, (UsefulBufC){frame->buf, len}, QCBOR_DECODE_MODE_NORMAL);
    QCBORDecode_EnterMap(&context, NULL);
    if (QCBORDecode_GetAndResetError(&context) != QCBOR_SUCCESS) return -1;

    QCBORError err;
    uint64_t uval;
    UsefulBufC text;

    err = get_optional_uint(&context, GW_KEY_SETTINGS_SEQUENCE, &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval > UINT16_MAX) return -1;
        out->sequence = (uint16_t)uval;
        out->has_sequence = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_text(&context, GW_KEY_SETTINGS_ID, &text);
    if (err == QCBOR_SUCCESS) {
        if (text.len == 0 || copy_text(out->setting_id, sizeof(out->setting_id), text) != 0) {
            return -1;
        }
        out->has_setting_id = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_text(&context, GW_KEY_SETTINGS_TITLE, &text);
    if (err == QCBOR_SUCCESS) {
        if (copy_text(out->title, sizeof(out->title), text) != 0) return -1;
        out->has_title = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_text(&context, GW_KEY_SETTINGS_GROUP, &text);
    if (err == QCBOR_SUCCESS) {
        if (copy_text(out->group, sizeof(out->group), text) != 0) return -1;
        out->has_group = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_text(&context, GW_KEY_SETTINGS_UNIT, &text);
    if (err == QCBOR_SUCCESS) {
        if (copy_text(out->unit, sizeof(out->unit), text) != 0) return -1;
        out->has_unit = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_uint(&context, GW_KEY_SETTINGS_TYPE, &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval == GW_SETTING_TYPE_NONE || uval > GW_SETTING_TYPE_ENUM) return -1;
        out->type = (uint8_t)uval;
        out->value.type = (uint8_t)uval;
        out->has_type = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_uint(&context, GW_KEY_SETTINGS_FLAGS, &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval > UINT16_MAX) return -1;
        out->flags = (uint16_t)uval;
        out->has_flags = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_uint(&context, GW_KEY_SETTINGS_MAX_LENGTH, &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval > UINT16_MAX) return -1;
        out->max_length = (uint16_t)uval;
        out->has_max_length = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_uint(&context, GW_KEY_SETTINGS_OPTION_INDEX, &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval > UINT8_MAX) return -1;
        out->option_index = (uint8_t)uval;
        out->has_option_index = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_uint(&context, GW_KEY_SETTINGS_OPTION_COUNT, &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval > UINT16_MAX) return -1;
        out->option_count = (uint16_t)uval;
        out->has_option_count = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_uint(&context, GW_KEY_SETTINGS_TRANSACTION_ID,
                            &out->transaction_id);
    if (err == QCBOR_SUCCESS) out->has_transaction_id = true;
    else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_uint(&context, GW_KEY_SETTINGS_EXPECTED_REVISION, &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval > UINT32_MAX) return -1;
        out->expected_revision = (uint32_t)uval;
        out->has_expected_revision = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    err = get_optional_uint(&context, GW_KEY_SETTINGS_NEW_REVISION, &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval > UINT32_MAX) return -1;
        out->new_revision = (uint32_t)uval;
        out->has_new_revision = true;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    if (out->has_type) {
        int64_t ival;
        switch (out->type) {
        case GW_SETTING_TYPE_BOOL:
            QCBORDecode_GetBoolInMapN(&context, GW_KEY_SETTINGS_VALUE,
                                      &out->value.value.bool_val);
            err = QCBORDecode_GetAndResetError(&context);
            break;
        case GW_SETTING_TYPE_INT:
            err = get_optional_int(&context, GW_KEY_SETTINGS_VALUE, &ival);
            if (err == QCBOR_SUCCESS) {
                if (ival < INT32_MIN || ival > INT32_MAX) return -1;
                out->value.value.int_val = (int32_t)ival;
            }
            break;
        case GW_SETTING_TYPE_STRING:
            err = get_optional_text(&context, GW_KEY_SETTINGS_VALUE, &text);
            if (err == QCBOR_SUCCESS &&
                copy_text(out->value.value.string_val,
                          sizeof(out->value.value.string_val), text) != 0) {
                return -1;
            }
            break;
        case GW_SETTING_TYPE_ENUM:
            err = get_optional_uint(&context, GW_KEY_SETTINGS_VALUE, &uval);
            if (err == QCBOR_SUCCESS) {
                if (uval > UINT8_MAX) return -1;
                out->value.value.enum_val = (uint8_t)uval;
            }
            break;
        default:
            err = QCBOR_ERR_LABEL_NOT_FOUND;
            break;
        }
        if (err == QCBOR_SUCCESS) out->has_value = true;
        else if (err != QCBOR_ERR_LABEL_NOT_FOUND) return -1;
    }

    QCBORDecode_ExitMap(&context);
    return QCBORDecode_Finish(&context) == QCBOR_SUCCESS ? 0 : -1;
}

int gw_settings_frame_view_parse(const gw_settings_frame_view_t *view,
                                 size_t len, gw_settings_snapshot_t *out)
{
    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    if (parse_settings_view(view, len, &out->view) != 0 || !out->view.has_setting_id ||
        !out->view.has_type) return -1;

    out->settings_count = 1;
    gw_settings_entry_t *entry = &out->entries[0];
    if (copy_text(out->text_storage[0], sizeof(out->text_storage[0]),
                  (UsefulBufC){out->view.setting_id, strlen(out->view.setting_id)}) != 0) {
        return -1;
    }
    entry->setting_id = out->text_storage[0];
    entry->setting_type = out->view.type;
    entry->flags = out->view.flags;
    entry->writable = !out->view.has_flags ||
                      !(out->view.flags & GW_SETTING_FLAG_READONLY);
    entry->has_value = out->view.has_value;
    if (out->view.has_group) {
        if (copy_text(out->text_storage[1], sizeof(out->text_storage[1]),
                      (UsefulBufC){out->view.group, strlen(out->view.group)}) != 0) {
            return -1;
        }
        entry->group = out->text_storage[1];
    }
    entry->string_val.max_len = out->view.max_length;

    if (entry->has_value) {
        switch (entry->setting_type) {
        case GW_SETTING_TYPE_BOOL: entry->bool_val = out->view.value.value.bool_val; break;
        case GW_SETTING_TYPE_INT: entry->int_val = out->view.value.value.int_val; break;
        case GW_SETTING_TYPE_STRING:
            if (copy_text(out->text_storage[2], sizeof(out->text_storage[2]),
                          (UsefulBufC){out->view.value.value.string_val,
                                       strlen(out->view.value.value.string_val)}) != 0) {
                return -1;
            }
            entry->string_val.str = out->text_storage[2];
            break;
        case GW_SETTING_TYPE_ENUM: entry->enum_val.value = out->view.value.value.enum_val; break;
        default: break;
        }
    }
    return 0;
}

int gw_settings_encode_request(const char *command, const char *device_id,
                               uint8_t *out_buf, size_t out_buf_cap)
{
    if (command == NULL || command[0] == '\0' || device_id == NULL ||
        device_id[0] == '\0' || out_buf == NULL || out_buf_cap == 0) return -1;
    gw_message_t msg = {.protocol_version = GW_PROTOCOL_VERSION, .has_device_id = 1};
    strlcpy(msg.type, command, sizeof(msg.type));
    strlcpy(msg.device_id, device_id, sizeof(msg.device_id));
    strlcpy(msg.command, command, sizeof(msg.command));
    return cbor_codec_encode(&msg, out_buf, out_buf_cap);
}

int gw_settings_value_frame_view_parse(const gw_settings_frame_view_t *view,
                                       size_t len, gw_settings_value_entry_t *out)
{
    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    gw_settings_view_t parsed;
    if (parse_settings_view(view, len, &parsed) != 0 || !parsed.has_setting_id ||
        !parsed.has_type) return -1;
    if (strlcpy(out->setting_id_storage, parsed.setting_id,
                sizeof(out->setting_id_storage)) >= sizeof(out->setting_id_storage)) {
        return -1;
    }
    out->setting_id = out->setting_id_storage;
    out->setting_type = parsed.type;
    out->has_value = parsed.has_value;
    if (!parsed.has_value) return 0;
    switch (parsed.type) {
    case GW_SETTING_TYPE_BOOL: out->bool_val = parsed.value.value.bool_val; break;
    case GW_SETTING_TYPE_INT: out->int_val = parsed.value.value.int_val; break;
    case GW_SETTING_TYPE_STRING:
        if (strlcpy(out->string_val.storage, parsed.value.value.string_val,
                    sizeof(out->string_val.storage)) >= sizeof(out->string_val.storage)) {
            return -1;
        }
        out->string_val.str = out->string_val.storage;
        break;
    case GW_SETTING_TYPE_ENUM: out->enum_val = parsed.value.value.enum_val; break;
    default: return -1;
    }
    return 0;
}
