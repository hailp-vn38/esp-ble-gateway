#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "qcbor/qcbor_decode.h"
#include "qcbor/qcbor_encode.h"
#include "qcbor/qcbor_spiffy_decode.h"

#include "gw_settings_view.h"

/* ── Internal helpers ──────────────────────────────────────────────── */

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

static QCBORError get_optional_bool(QCBORDecodeContext *ctx, int64_t key,
                                    bool *value)
{
    *value = false;
    QCBORDecode_GetBoolInMapN(ctx, key, value);
    return QCBORDecode_GetAndResetError(ctx);
}

/* ── Frame view parser ─────────────────────────────────────────────── */

int gw_settings_frame_view_parse(const gw_settings_frame_view_t *view,
                                 size_t len,
                                 gw_settings_snapshot_t *out)
{
    if (view == NULL || view->buf == NULL || len == 0 || out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    QCBORDecodeContext context;
    QCBORDecode_Init(&context, (UsefulBufC){view->buf, len},
                     QCBOR_DECODE_MODE_NORMAL);
    QCBORDecode_EnterMap(&context, NULL);
    if (QCBORDecode_GetAndResetError(&context) != QCBOR_SUCCESS) {
        return -1;
    }

    /* Config revision (optional). */
    uint64_t uval = 0;
    QCBORError err = get_optional_uint(&context,
                                       GW_SETTINGS_CBOR_KEY_CONFIG_REVISION,
                                       &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval > UINT32_MAX) return -1;
        out->config_revision = (uint32_t)uval;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    /* Settings count (optional, computed from entries). */
    err = get_optional_uint(&context, GW_SETTINGS_CBOR_KEY_SETTINGS_ITEM,
                            &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval > GW_SETTINGS_MAX_ENTRIES) return -1;
        out->settings_count = (uint16_t)uval;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    /* Setting ID (required). */
    UsefulBufC text_val;
    err = get_optional_text(&context, GW_SETTINGS_CBOR_KEY_SETTING_ID,
                            &text_val);
    if (err != QCBOR_SUCCESS || text_val.len == 0 ||
        text_val.len >= GW_SETTINGS_MAX_ID_LEN) {
        return -1;
    }
    /* Point directly into the frame buffer (zero-copy). */
    out->entries[0].setting_id = (const char *)text_val.ptr;

    /* Setting type (required). */
    err = get_optional_uint(&context, GW_SETTINGS_CBOR_KEY_SETTING_TYPE, &uval);
    if (err != QCBOR_SUCCESS || uval > GW_SETTINGS_TYPE_ENUM) {
        return -1;
    }
    out->entries[0].setting_type = (uint8_t)uval;

    /* Writable (optional, default false). */
    bool writable = false;
    err = get_optional_bool(&context, GW_SETTINGS_CBOR_KEY_WRITABLE, &writable);
    if (err != QCBOR_SUCCESS && err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }
    out->entries[0].writable = writable;

    /* Default value (optional). */
    int64_t ival = 0;
    err = get_optional_int(&context, GW_SETTINGS_CBOR_KEY_DEFAULT_VALUE, &ival);
    if (err == QCBOR_SUCCESS) {
        out->entries[0].has_value = true;
        switch (out->entries[0].setting_type) {
        case GW_SETTINGS_TYPE_BOOL:
            out->entries[0].bool_val = (ival != 0);
            break;
        case GW_SETTINGS_TYPE_INT:
            if (ival < INT32_MIN || ival > INT32_MAX) return -1;
            out->entries[0].int_val = (int32_t)ival;
            break;
        case GW_SETTINGS_TYPE_ENUM:
            if (ival < INT32_MIN || ival > INT32_MAX) return -1;
            out->entries[0].enum_val.value = (int32_t)ival;
            break;
        default:
            break;
        }
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    /* Numeric range (optional). */
    err = get_optional_int(&context, GW_SETTINGS_CBOR_KEY_MIN_VALUE, &ival);
    if (err == QCBOR_SUCCESS) {
        if (ival < INT32_MIN || ival > INT32_MAX) return -1;
        out->entries[0].numeric_range.min_value = (int32_t)ival;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    err = get_optional_int(&context, GW_SETTINGS_CBOR_KEY_MAX_VALUE, &ival);
    if (err == QCBOR_SUCCESS) {
        if (ival < INT32_MIN || ival > INT32_MAX) return -1;
        out->entries[0].numeric_range.max_value = (int32_t)ival;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    err = get_optional_uint(&context, GW_SETTINGS_CBOR_KEY_STEP, &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval > UINT32_MAX) return -1;
        out->entries[0].numeric_range.step = (uint32_t)uval;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    /* Enum options (optional array of maps). */
    QCBORDecode_EnterArrayFromMapN(&context, GW_SETTINGS_CBOR_KEY_ENUM_OPTIONS);
    err = QCBORDecode_GetAndResetError(&context);
    if (err == QCBOR_SUCCESS) {
        uint8_t idx = 0;
        /* Iterate array entries.  Peek into each map to read fields, then
         * exit the map before the next iteration.  When PeekNext returns
         * QCBOR_ERR_NO_MORE_ITEMS the array is exhausted. */
        while (idx < GW_SETTINGS_MAX_ENUM_OPTIONS) {
            QCBORDecode_PeekNext(&context, NULL);
            err = QCBORDecode_GetAndResetError(&context);
            if (err == QCBOR_ERR_NO_MORE_ITEMS) break;
            if (err != QCBOR_SUCCESS) return -1;

            QCBORDecode_EnterMap(&context, NULL);
            err = QCBORDecode_GetAndResetError(&context);
            if (err != QCBOR_SUCCESS) return -1;

            int64_t ev = 0;
            QCBORDecode_GetInt64InMapN(&context,
                                       GW_SETTINGS_CBOR_KEY_ENUM_VALUE, &ev);
            err = QCBORDecode_GetAndResetError(&context);
            if (err != QCBOR_SUCCESS) {
                QCBORDecode_ExitMap(&context);
                return -1;
            }
            if (ev < INT32_MIN || ev > INT32_MAX) {
                QCBORDecode_ExitMap(&context);
                return -1;
            }
            out->entries[0].enum_options[idx].value = (int32_t)ev;

            UsefulBufC label_buf;
            QCBORDecode_GetTextStringInMapN(&context,
                                            GW_SETTINGS_CBOR_KEY_ENUM_LABEL,
                                            &label_buf);
            err = QCBORDecode_GetAndResetError(&context);
            if (err == QCBOR_SUCCESS && label_buf.len > 0) {
                out->entries[0].enum_options[idx].label =
                    (const char *)label_buf.ptr;
            } else if (err == QCBOR_SUCCESS) {
                out->entries[0].enum_options[idx].label = "";
            } else {
                QCBORDecode_ExitMap(&context);
                return -1;
            }

            QCBORDecode_ExitMap(&context);
            idx++;
        }
        QCBORDecode_ExitArray(&context);
        out->entries[0].enum_option_count = idx;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    /* Group (optional). */
    err = get_optional_text(&context, GW_SETTINGS_CBOR_KEY_GROUP, &text_val);
    if (err == QCBOR_SUCCESS) {
        if (text_val.len > 0 && text_val.len < GW_SETTINGS_MAX_GROUP_LEN) {
            out->entries[0].group = (const char *)text_val.ptr;
        }
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    err = get_optional_uint(&context, GW_SETTINGS_CBOR_KEY_GROUP_ORDER, &uval);
    if (err == QCBOR_SUCCESS) {
        if (uval > UINT8_MAX) return -1;
        out->entries[0].group_order = (uint8_t)uval;
    } else if (err != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    /* Unknown keys are silently ignored (targeted-lookup pattern). */

    QCBORDecode_ExitMap(&context);
    if (QCBORDecode_Finish(&context) != QCBOR_SUCCESS) return -1;

    return 0;
}

/* ── Settings request encoder ──────────────────────────────────────── */

int gw_settings_encode_request(const char *command,
                               const char *device_id,
                               uint8_t *out_buf,
                               size_t out_buf_cap)
{
    if (command == NULL || command[0] == '\0' ||
        device_id == NULL || device_id[0] == '\0' ||
        out_buf == NULL || out_buf_cap == 0) {
        return -1;
    }

    gw_message_t msg = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .has_device_id = 1,
    };
    strlcpy(msg.type, command, sizeof(msg.type));
    strlcpy(msg.device_id, device_id, sizeof(msg.device_id));
    strlcpy(msg.command, command, sizeof(msg.command));

    return cbor_codec_encode(&msg, out_buf, out_buf_cap);
}
