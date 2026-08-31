#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "qcbor/qcbor_decode.h"
#include "qcbor/qcbor_encode.h"
#include "qcbor/qcbor_spiffy_decode.h"

#include "cbor_codec.h"

static const char *TAG = "cbor_codec";

enum {
    CBOR_KEY_PROTOCOL_VERSION = 0,
    CBOR_KEY_TYPE = 1,
    CBOR_KEY_DEVICE_ID = 2,
    CBOR_KEY_COMMAND = 3,
    CBOR_KEY_INT_VALUE = 4,
    CBOR_KEY_BOOL_VALUE = 5,
    CBOR_KEY_NAME = 6,
    /* 7 reserved since v4 (former device_type key). Never emitted by the
     * encoder; targeted-lookup decoder ignores it if present. Do not
     * renumber. */
    CBOR_KEY_RESERVED_7 = 7,
    CBOR_KEY_BLE_ADDR = 8,
    CBOR_KEY_BLE_ADDR_TYPE = 9,
    CBOR_KEY_REQUEST_ID = 10,
    CBOR_KEY_SNAPSHOT_ID = 11,
    CBOR_KEY_SEQUENCE = 12,
    CBOR_KEY_TOTAL = 13,
    CBOR_KEY_VALUE_TYPE = 14,
    CBOR_KEY_CAPABILITY_FLAGS = 15,
    CBOR_KEY_MIN_VALUE = 16,
    CBOR_KEY_MAX_VALUE = 17,
    CBOR_KEY_STEP = 18,
    CBOR_KEY_CAPABILITY_LABEL = 19,
    CBOR_KEY_CAPABILITY_UNIT = 20,
    CBOR_KEY_CAPABILITY_REVISION = 21,
    CBOR_KEY_FEATURE_ID = 22,
    CBOR_KEY_FEATURE_TYPE = 23,
    CBOR_KEY_FEATURE_SCHEMA_VERSION = 24,
    CBOR_KEY_FEATURE_FLAGS = 25,
    CBOR_KEY_PROPERTY_ID = 26,
    CBOR_KEY_FEATURE_VALUE_BOOL = 27,
    CBOR_KEY_FEATURE_VALUE_INT = 28,
    CBOR_KEY_FEATURE_TOOL = 29,
    CBOR_KEY_FEATURE_TOTAL = 30,
};

static bool valid_string(const char *value, size_t capacity, bool allow_empty)
{
    size_t length = strnlen(value, capacity);
    return length < capacity && (allow_empty || length > 0);
}

static int copy_text(UsefulBufC value, char *destination, size_t capacity,
                     bool allow_empty)
{
    if (value.len >= capacity || (!allow_empty && value.len == 0)) return -1;
    if (value.len > 0) memcpy(destination, value.ptr, value.len);
    destination[value.len] = '\0';
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_ble_addr(const char *text, uint8_t address[6])
{
    if (text == NULL || address == NULL) return -1;
    uint8_t display_order[6];
    for (int i = 0; i < 6; i++) {
        int high = hex_value(*text++);
        int low = high >= 0 ? hex_value(*text++) : -1;
        if (high < 0 || low < 0) return -1;
        display_order[i] = (uint8_t)((high << 4) | low);
        if (i < 5) {
            if (*text != ':' && *text != '-') return -1;
            text++;
        }
    }
    if (*text != '\0') return -1;
    for (int i = 0; i < 6; i++) address[i] = display_order[5 - i];
    return 0;
}

static void format_ble_addr(const uint8_t address[6], char output[18])
{
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[5], address[4], address[3], address[2], address[1], address[0]);
}

int cbor_codec_encode(const gw_message_t *msg, uint8_t *out_buf, size_t out_buf_cap)
{
    if (msg == NULL || out_buf == NULL || out_buf_cap == 0 ||
        !valid_string(msg->type, sizeof(msg->type), false) ||
        !valid_string(msg->command, sizeof(msg->command), false) ||
        (msg->has_device_id && !valid_string(msg->device_id, sizeof(msg->device_id), false)) ||
        !valid_string(msg->name, sizeof(msg->name), true) ||
        !valid_string(msg->capability_label,
                      sizeof(msg->capability_label), true) ||
        !valid_string(msg->capability_unit,
                      sizeof(msg->capability_unit), true) ||
        !valid_string(msg->feature_id, sizeof(msg->feature_id), true) ||
        !valid_string(msg->feature_tool, sizeof(msg->feature_tool), true)) {
        return -1;
    }

    // Strict v4: the encoder only emits the current protocol version.
    if (msg->protocol_version != GW_PROTOCOL_VERSION) {
        ESP_LOGE(TAG, "Unsupported protocol version: %u",
                 (unsigned)msg->protocol_version);
        return -1;
    }

    QCBOREncodeContext context;
    QCBOREncode_Init(&context, (UsefulBuf){out_buf, out_buf_cap});
    QCBOREncode_OpenMap(&context);
    QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_PROTOCOL_VERSION,
                                msg->protocol_version);
    QCBOREncode_AddSZStringToMapN(&context, CBOR_KEY_TYPE, msg->type);
    if (msg->has_device_id) {
        QCBOREncode_AddSZStringToMapN(&context, CBOR_KEY_DEVICE_ID, msg->device_id);
    }
    QCBOREncode_AddSZStringToMapN(&context, CBOR_KEY_COMMAND, msg->command);
    if (msg->has_request_id) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_REQUEST_ID, msg->request_id);
    }
    QCBOREncode_AddInt64ToMapN(&context, CBOR_KEY_INT_VALUE, msg->int_value);
    QCBOREncode_AddBoolToMapN(&context, CBOR_KEY_BOOL_VALUE, msg->bool_value != 0);
    if (msg->name[0] != '\0') {
        QCBOREncode_AddSZStringToMapN(&context, CBOR_KEY_NAME, msg->name);
    }
    // Key 7 (CBOR_KEY_RESERVED_7) is never emitted since v4.
    if (msg->has_ble_addr) {
        QCBOREncode_AddBytesToMapN(&context, CBOR_KEY_BLE_ADDR,
                                   (UsefulBufC){msg->ble_addr, sizeof(msg->ble_addr)});
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_BLE_ADDR_TYPE,
                                    msg->ble_addr_type);
    }
    if (msg->has_snapshot_id) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_SNAPSHOT_ID,
                                    msg->snapshot_id);
    }
    if (msg->has_sequence) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_SEQUENCE,
                                    msg->sequence);
    }
    if (msg->has_total) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_TOTAL, msg->total);
    }
    if (msg->has_value_type) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_VALUE_TYPE,
                                    msg->value_type);
    }
    if (msg->has_capability_flags) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_CAPABILITY_FLAGS,
                                    msg->capability_flags);
    }
    if (msg->has_min_value) {
        QCBOREncode_AddInt64ToMapN(&context, CBOR_KEY_MIN_VALUE,
                                   msg->min_value);
    }
    if (msg->has_max_value) {
        QCBOREncode_AddInt64ToMapN(&context, CBOR_KEY_MAX_VALUE,
                                   msg->max_value);
    }
    if (msg->has_step) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_STEP, msg->step);
    }
    if (msg->capability_label[0] != '\0') {
        QCBOREncode_AddSZStringToMapN(&context, CBOR_KEY_CAPABILITY_LABEL,
                                      msg->capability_label);
    }
    if (msg->capability_unit[0] != '\0') {
        QCBOREncode_AddSZStringToMapN(&context, CBOR_KEY_CAPABILITY_UNIT,
                                      msg->capability_unit);
    }
    if (msg->has_capability_revision) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_CAPABILITY_REVISION,
                                    msg->capability_revision);
    }
    if (msg->has_feature_id) {
        QCBOREncode_AddSZStringToMapN(&context, CBOR_KEY_FEATURE_ID,
                                      msg->feature_id);
    }
    if (msg->has_feature_type) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_FEATURE_TYPE,
                                    msg->feature_type);
    }
    if (msg->has_feature_schema_version) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_FEATURE_SCHEMA_VERSION,
                                    msg->feature_schema_version);
    }
    if (msg->has_feature_flags) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_FEATURE_FLAGS,
                                    msg->feature_flags);
    }
    if (msg->has_property_id) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_PROPERTY_ID,
                                    msg->property_id);
    }
    if (msg->has_feature_value_bool) {
        QCBOREncode_AddBoolToMapN(&context, CBOR_KEY_FEATURE_VALUE_BOOL,
                                  msg->feature_value_bool);
    }
    if (msg->has_feature_value_int) {
        QCBOREncode_AddInt64ToMapN(&context, CBOR_KEY_FEATURE_VALUE_INT,
                                   msg->feature_value_int);
    }
    if (msg->has_feature_tool) {
        QCBOREncode_AddSZStringToMapN(&context, CBOR_KEY_FEATURE_TOOL,
                                      msg->feature_tool);
    }
    if (msg->has_feature_total) {
        QCBOREncode_AddUInt64ToMapN(&context, CBOR_KEY_FEATURE_TOTAL,
                                    msg->feature_total);
    }
    QCBOREncode_CloseMap(&context);

    UsefulBufC encoded;
    QCBORError error = QCBOREncode_Finish(&context, &encoded);
    if (error != QCBOR_SUCCESS) {
        ESP_LOGE(TAG, "CBOR encode failed: %d", error);
        return -1;
    }
    return (int)encoded.len;
}

static QCBORError get_optional_text(QCBORDecodeContext *context, int64_t key,
                                    UsefulBufC *value)
{
    *value = NULLUsefulBufC;
    QCBORDecode_GetTextStringInMapN(context, key, value);
    return QCBORDecode_GetAndResetError(context);
}

static QCBORError get_optional_uint(QCBORDecodeContext *context, int64_t key,
                                    uint64_t *value)
{
    *value = 0;
    QCBORDecode_GetUInt64InMapN(context, key, value);
    return QCBORDecode_GetAndResetError(context);
}

static QCBORError get_optional_int(QCBORDecodeContext *context, int64_t key,
                                   int64_t *value)
{
    *value = 0;
    QCBORDecode_GetInt64InMapN(context, key, value);
    return QCBORDecode_GetAndResetError(context);
}

int cbor_codec_decode(const uint8_t *buf, size_t len, gw_message_t *out_msg)
{
    if (buf == NULL || out_msg == NULL || len == 0 || len > GW_MSG_MAX_LEN) return -1;
    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->protocol_version = GW_PROTOCOL_VERSION;

    QCBORDecodeContext context;
    QCBORDecode_Init(&context, (UsefulBufC){buf, len}, QCBOR_DECODE_MODE_NORMAL);
    QCBORDecode_EnterMap(&context, NULL);
    if (QCBORDecode_GetAndResetError(&context) != QCBOR_SUCCESS) return -1;

    uint64_t protocol_version = 0;
    bool has_protocol_version = false;
    QCBORDecode_GetUInt64InMapN(&context, CBOR_KEY_PROTOCOL_VERSION, &protocol_version);
    QCBORError error = QCBORDecode_GetAndResetError(&context);
    if (error == QCBOR_SUCCESS) {
        has_protocol_version = true;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }
    // Strict v4: the version must be explicit and exact; v1/v2/v3 reject.
    if (!has_protocol_version || protocol_version != GW_PROTOCOL_VERSION) {
        ESP_LOGE(TAG, "Unsupported protocol version: %llu",
                 (unsigned long long)protocol_version);
        return -1;
    }

    UsefulBufC type_value = NULLUsefulBufC;
    QCBORDecode_GetTextStringInMapN(&context, CBOR_KEY_TYPE, &type_value);
    if (QCBORDecode_GetAndResetError(&context) != QCBOR_SUCCESS ||
        copy_text(type_value, out_msg->type, sizeof(out_msg->type), false) != 0) {
        return -1;
    }

    UsefulBufC command_value = NULLUsefulBufC;
    QCBORDecode_GetTextStringInMapN(&context, CBOR_KEY_COMMAND, &command_value);
    if (QCBORDecode_GetAndResetError(&context) != QCBOR_SUCCESS ||
        copy_text(command_value, out_msg->command, sizeof(out_msg->command), false) != 0) {
        return -1;
    }

    int64_t int_value = 0;
    QCBORDecode_GetInt64InMapN(&context, CBOR_KEY_INT_VALUE, &int_value);
    if (QCBORDecode_GetAndResetError(&context) != QCBOR_SUCCESS ||
        int_value < INT_MIN || int_value > INT_MAX) {
        return -1;
    }

    bool bool_value = false;
    QCBORDecode_GetBoolInMapN(&context, CBOR_KEY_BOOL_VALUE, &bool_value);
    if (QCBORDecode_GetAndResetError(&context) != QCBOR_SUCCESS) return -1;

    uint64_t request_id = 0;
    QCBORDecode_GetUInt64InMapN(&context, CBOR_KEY_REQUEST_ID, &request_id);
    error = QCBORDecode_GetAndResetError(&context);
    if (error == QCBOR_SUCCESS) {
        if (request_id == 0 || request_id > UINT32_MAX) return -1;
        out_msg->request_id = (uint32_t)request_id;
        out_msg->has_request_id = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    UsefulBufC optional_value;
    error = get_optional_text(&context, CBOR_KEY_DEVICE_ID, &optional_value);
    if (error == QCBOR_SUCCESS) {
        if (copy_text(optional_value, out_msg->device_id, sizeof(out_msg->device_id), false) != 0) {
            return -1;
        }
        out_msg->has_device_id = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    error = get_optional_text(&context, CBOR_KEY_NAME, &optional_value);
    if (error == QCBOR_SUCCESS) {
        if (copy_text(optional_value, out_msg->name, sizeof(out_msg->name), true) != 0) return -1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    // CBOR key 7 is reserved since v4: never looked up, therefore ignored.

    UsefulBufC address_value = NULLUsefulBufC;
    QCBORDecode_GetByteStringInMapN(&context, CBOR_KEY_BLE_ADDR, &address_value);
    error = QCBORDecode_GetAndResetError(&context);
    if (error == QCBOR_SUCCESS) {
        if (address_value.len != sizeof(out_msg->ble_addr)) return -1;
        memcpy(out_msg->ble_addr, address_value.ptr, sizeof(out_msg->ble_addr));
        uint64_t address_type = 0;
        QCBORDecode_GetUInt64InMapN(&context, CBOR_KEY_BLE_ADDR_TYPE, &address_type);
        error = QCBORDecode_GetAndResetError(&context);
        if (error != QCBOR_SUCCESS || address_type > UINT8_MAX) return -1;
        out_msg->ble_addr_type = (uint8_t)address_type;
        out_msg->has_ble_addr = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) {
        return -1;
    }

    uint64_t optional_uint = 0;
    error = get_optional_uint(&context, CBOR_KEY_SNAPSHOT_ID, &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint == 0 || optional_uint > UINT32_MAX) return -1;
        out_msg->snapshot_id = (uint32_t)optional_uint;
        out_msg->has_snapshot_id = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_uint(&context, CBOR_KEY_SEQUENCE, &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint > UINT16_MAX) return -1;
        out_msg->sequence = (uint16_t)optional_uint;
        out_msg->has_sequence = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_uint(&context, CBOR_KEY_TOTAL, &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint > UINT16_MAX) return -1;
        out_msg->total = (uint16_t)optional_uint;
        out_msg->has_total = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_uint(&context, CBOR_KEY_VALUE_TYPE, &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint > UINT8_MAX) return -1;
        out_msg->value_type = (uint8_t)optional_uint;
        out_msg->has_value_type = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_uint(&context, CBOR_KEY_CAPABILITY_FLAGS,
                              &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint > UINT8_MAX) return -1;
        out_msg->capability_flags = (uint8_t)optional_uint;
        out_msg->has_capability_flags = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    int64_t optional_int = 0;
    error = get_optional_int(&context, CBOR_KEY_MIN_VALUE, &optional_int);
    if (error == QCBOR_SUCCESS) {
        if (optional_int < INT32_MIN || optional_int > INT32_MAX) return -1;
        out_msg->min_value = (int32_t)optional_int;
        out_msg->has_min_value = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_int(&context, CBOR_KEY_MAX_VALUE, &optional_int);
    if (error == QCBOR_SUCCESS) {
        if (optional_int < INT32_MIN || optional_int > INT32_MAX) return -1;
        out_msg->max_value = (int32_t)optional_int;
        out_msg->has_max_value = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_uint(&context, CBOR_KEY_STEP, &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint > UINT32_MAX) return -1;
        out_msg->step = (uint32_t)optional_uint;
        out_msg->has_step = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_text(&context, CBOR_KEY_CAPABILITY_LABEL,
                              &optional_value);
    if (error == QCBOR_SUCCESS) {
        if (copy_text(optional_value, out_msg->capability_label,
                      sizeof(out_msg->capability_label), true) != 0) return -1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_text(&context, CBOR_KEY_CAPABILITY_UNIT,
                              &optional_value);
    if (error == QCBOR_SUCCESS) {
        if (copy_text(optional_value, out_msg->capability_unit,
                      sizeof(out_msg->capability_unit), true) != 0) return -1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_uint(&context, CBOR_KEY_CAPABILITY_REVISION,
                              &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint > UINT32_MAX) return -1;
        out_msg->capability_revision = (uint32_t)optional_uint;
        out_msg->has_capability_revision = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    // Protocol v4 semantic feature fields.
    error = get_optional_text(&context, CBOR_KEY_FEATURE_ID, &optional_value);
    if (error == QCBOR_SUCCESS) {
        if (copy_text(optional_value, out_msg->feature_id,
                      sizeof(out_msg->feature_id), false) != 0) return -1;
        out_msg->has_feature_id = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_uint(&context, CBOR_KEY_FEATURE_TYPE, &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint > UINT8_MAX) return -1;
        out_msg->feature_type = (uint8_t)optional_uint;
        out_msg->has_feature_type = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_uint(&context, CBOR_KEY_FEATURE_SCHEMA_VERSION,
                              &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint > UINT16_MAX) return -1;
        out_msg->feature_schema_version = (uint16_t)optional_uint;
        out_msg->has_feature_schema_version = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_uint(&context, CBOR_KEY_FEATURE_FLAGS, &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint > UINT16_MAX) return -1;
        out_msg->feature_flags = (uint16_t)optional_uint;
        out_msg->has_feature_flags = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_uint(&context, CBOR_KEY_PROPERTY_ID, &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint > UINT8_MAX) return -1;
        out_msg->property_id = (uint8_t)optional_uint;
        out_msg->has_property_id = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    bool optional_bool = false;
    QCBORDecode_GetBoolInMapN(&context, CBOR_KEY_FEATURE_VALUE_BOOL, &optional_bool);
    error = QCBORDecode_GetAndResetError(&context);
    if (error == QCBOR_SUCCESS) {
        out_msg->feature_value_bool = optional_bool;
        out_msg->has_feature_value_bool = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_int(&context, CBOR_KEY_FEATURE_VALUE_INT, &optional_int);
    if (error == QCBOR_SUCCESS) {
        if (optional_int < INT32_MIN || optional_int > INT32_MAX) return -1;
        out_msg->feature_value_int = (int32_t)optional_int;
        out_msg->has_feature_value_int = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_text(&context, CBOR_KEY_FEATURE_TOOL, &optional_value);
    if (error == QCBOR_SUCCESS) {
        if (copy_text(optional_value, out_msg->feature_tool,
                      sizeof(out_msg->feature_tool), false) != 0) return -1;
        out_msg->has_feature_tool = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    error = get_optional_uint(&context, CBOR_KEY_FEATURE_TOTAL, &optional_uint);
    if (error == QCBOR_SUCCESS) {
        if (optional_uint > UINT16_MAX) return -1;
        out_msg->feature_total = (uint16_t)optional_uint;
        out_msg->has_feature_total = 1;
    } else if (error != QCBOR_ERR_LABEL_NOT_FOUND) return -1;

    QCBORDecode_ExitMap(&context);
    if (QCBORDecode_Finish(&context) != QCBOR_SUCCESS) return -1;

    out_msg->protocol_version = (uint8_t)protocol_version;
    out_msg->int_value = (int)int_value;
    out_msg->bool_value = bool_value;
    out_msg->has_int_value = 1;
    out_msg->has_bool_value = 1;
    return 0;
}

int cbor_codec_msg_to_json(const gw_message_t *msg, char *out_json, size_t out_json_cap)
{
    if (msg == NULL || out_json == NULL || out_json_cap == 0) return -1;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return -1;
    cJSON_AddNumberToObject(root, "protocol_version", msg->protocol_version);
    cJSON_AddStringToObject(root, "type", msg->type);
    if (msg->has_device_id) cJSON_AddStringToObject(root, "device_id", msg->device_id);
    cJSON_AddStringToObject(root, "command", msg->command);
    if (msg->has_request_id) cJSON_AddNumberToObject(root, "request_id", msg->request_id);
    cJSON_AddNumberToObject(root, "int_value", msg->int_value);
    cJSON_AddBoolToObject(root, "bool_value", msg->bool_value != 0);
    if (msg->name[0] != '\0') cJSON_AddStringToObject(root, "name", msg->name);
    if (msg->has_ble_addr) {
        char address[18];
        format_ble_addr(msg->ble_addr, address);
        cJSON_AddStringToObject(root, "ble_addr", address);
        cJSON_AddNumberToObject(root, "ble_addr_type", msg->ble_addr_type);
    }
    if (msg->has_snapshot_id) {
        cJSON_AddNumberToObject(root, "snapshot_id", msg->snapshot_id);
    }
    if (msg->has_sequence) cJSON_AddNumberToObject(root, "sequence", msg->sequence);
    if (msg->has_total) cJSON_AddNumberToObject(root, "total", msg->total);
    if (msg->has_value_type) cJSON_AddNumberToObject(root, "value_type", msg->value_type);
    if (msg->has_capability_flags) {
        cJSON_AddNumberToObject(root, "capability_flags", msg->capability_flags);
    }
    if (msg->has_min_value) cJSON_AddNumberToObject(root, "min_value", msg->min_value);
    if (msg->has_max_value) cJSON_AddNumberToObject(root, "max_value", msg->max_value);
    if (msg->has_step) cJSON_AddNumberToObject(root, "step", msg->step);
    if (msg->capability_label[0] != '\0') {
        cJSON_AddStringToObject(root, "capability_label", msg->capability_label);
    }
    if (msg->capability_unit[0] != '\0') {
        cJSON_AddStringToObject(root, "capability_unit", msg->capability_unit);
    }
    if (msg->has_capability_revision) {
        cJSON_AddNumberToObject(root, "capability_revision",
                               msg->capability_revision);
    }
    if (msg->has_feature_id) cJSON_AddStringToObject(root, "feature_id", msg->feature_id);
    if (msg->has_feature_type) cJSON_AddNumberToObject(root, "feature_type", msg->feature_type);
    if (msg->has_feature_schema_version) {
        cJSON_AddNumberToObject(root, "feature_schema_version",
                                msg->feature_schema_version);
    }
    if (msg->has_feature_flags) cJSON_AddNumberToObject(root, "feature_flags", msg->feature_flags);
    if (msg->has_property_id) cJSON_AddNumberToObject(root, "property_id", msg->property_id);
    if (msg->has_feature_value_bool) {
        cJSON_AddBoolToObject(root, "feature_value_bool", msg->feature_value_bool);
    }
    if (msg->has_feature_value_int) {
        cJSON_AddNumberToObject(root, "feature_value_int", msg->feature_value_int);
    }
    if (msg->has_feature_tool) cJSON_AddStringToObject(root, "feature_tool", msg->feature_tool);
    if (msg->has_feature_total) cJSON_AddNumberToObject(root, "feature_total", msg->feature_total);

    bool printed = cJSON_PrintPreallocated(root, out_json, (int)out_json_cap, false);
    cJSON_Delete(root);
    return printed ? (int)strlen(out_json) : -1;
}

static int copy_json_string(const cJSON *root, const char *key, char *destination,
                            size_t capacity, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL && !required) return 1;
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        !valid_string(item->valuestring, capacity, !required)) {
        return -1;
    }
    strlcpy(destination, item->valuestring, capacity);
    return 0;
}

static int optional_json_integer(const cJSON *root, const char *key,
                                 int64_t minimum, uint64_t maximum,
                                 int64_t *value, bool *present)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    *present = item != NULL;
    if (item == NULL) return 0;
    if (!cJSON_IsNumber(item) || item->valuedouble < (double)minimum ||
        item->valuedouble > (double)maximum) {
        return -1;
    }
    int64_t integer = (int64_t)item->valuedouble;
    if (item->valuedouble != (double)integer) return -1;
    *value = integer;
    return 0;
}

int cbor_codec_json_to_msg(const char *json_str, gw_message_t *out_msg)
{
    if (json_str == NULL || out_msg == NULL) return -1;
    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->protocol_version = GW_PROTOCOL_VERSION;

    cJSON *root = cJSON_Parse(json_str);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }

    int result = -1;
    if (copy_json_string(root, "type", out_msg->type, sizeof(out_msg->type), true) != 0 ||
        copy_json_string(root, "command", out_msg->command,
                         sizeof(out_msg->command), true) != 0) {
        goto cleanup;
    }

    int field_result = copy_json_string(root, "device_id", out_msg->device_id,
                                        sizeof(out_msg->device_id), false);
    if (field_result < 0) goto cleanup;
    out_msg->has_device_id = field_result == 0 && out_msg->device_id[0] != '\0';

    if (copy_json_string(root, "name", out_msg->name, sizeof(out_msg->name), false) < 0) {
        goto cleanup;
    }

    const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
    if (protocol != NULL) {
        // Strict v4: only the current protocol version is accepted.
        if (!cJSON_IsNumber(protocol) ||
            protocol->valuedouble != (double)GW_PROTOCOL_VERSION ||
            protocol->valuedouble != (double)protocol->valueint) {
            goto cleanup;
        }
        out_msg->protocol_version = (uint8_t)protocol->valueint;
    }

    const cJSON *request_id_item = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    if (request_id_item != NULL) {
        if (!cJSON_IsNumber(request_id_item) || request_id_item->valuedouble < 1 ||
            request_id_item->valuedouble > UINT32_MAX ||
            request_id_item->valuedouble != (double)request_id_item->valueint) {
            goto cleanup;
        }
        out_msg->request_id = (uint32_t)request_id_item->valueint;
        out_msg->has_request_id = 1;
    }

    const cJSON *int_item = cJSON_GetObjectItemCaseSensitive(root, "int_value");
    if (int_item != NULL) {
        if (!cJSON_IsNumber(int_item) || int_item->valuedouble < INT_MIN ||
            int_item->valuedouble > INT_MAX ||
            int_item->valuedouble != (double)int_item->valueint) {
            goto cleanup;
        }
        out_msg->int_value = int_item->valueint;
        out_msg->has_int_value = 1;
    }

    const cJSON *bool_item = cJSON_GetObjectItemCaseSensitive(root, "bool_value");
    if (bool_item != NULL) {
        if (!cJSON_IsBool(bool_item)) goto cleanup;
        out_msg->bool_value = cJSON_IsTrue(bool_item);
        out_msg->has_bool_value = 1;
    }

    int64_t numeric = 0;
    bool present = false;
    if (optional_json_integer(root, "snapshot_id", 1, UINT32_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->snapshot_id = (uint32_t)numeric;
        out_msg->has_snapshot_id = 1;
    }
    if (optional_json_integer(root, "sequence", 0, UINT16_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->sequence = (uint16_t)numeric;
        out_msg->has_sequence = 1;
    }
    if (optional_json_integer(root, "total", 0, UINT16_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->total = (uint16_t)numeric;
        out_msg->has_total = 1;
    }
    if (optional_json_integer(root, "value_type", 0, UINT8_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->value_type = (uint8_t)numeric;
        out_msg->has_value_type = 1;
    }
    if (optional_json_integer(root, "capability_flags", 0, UINT8_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->capability_flags = (uint8_t)numeric;
        out_msg->has_capability_flags = 1;
    }
    if (optional_json_integer(root, "min_value", INT32_MIN, INT32_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->min_value = (int32_t)numeric;
        out_msg->has_min_value = 1;
    }
    if (optional_json_integer(root, "max_value", INT32_MIN, INT32_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->max_value = (int32_t)numeric;
        out_msg->has_max_value = 1;
    }
    if (optional_json_integer(root, "step", 0, UINT32_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->step = (uint32_t)numeric;
        out_msg->has_step = 1;
    }
    if (copy_json_string(root, "capability_label",
                         out_msg->capability_label,
                         sizeof(out_msg->capability_label), false) < 0 ||
        copy_json_string(root, "capability_unit", out_msg->capability_unit,
                         sizeof(out_msg->capability_unit), false) < 0) {
        goto cleanup;
    }
    if (optional_json_integer(root, "capability_revision", 0, UINT32_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->capability_revision = (uint32_t)numeric;
        out_msg->has_capability_revision = 1;
    }

    if (copy_json_string(root, "feature_id", out_msg->feature_id,
                         sizeof(out_msg->feature_id), false) < 0 ||
        copy_json_string(root, "feature_tool", out_msg->feature_tool,
                         sizeof(out_msg->feature_tool), false) < 0) {
        goto cleanup;
    }
    if (optional_json_integer(root, "feature_type", 0, UINT8_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->feature_type = (uint8_t)numeric;
        out_msg->has_feature_type = 1;
    }
    if (optional_json_integer(root, "feature_schema_version", 0, UINT16_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->feature_schema_version = (uint16_t)numeric;
        out_msg->has_feature_schema_version = 1;
    }
    if (optional_json_integer(root, "feature_flags", 0, UINT16_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->feature_flags = (uint16_t)numeric;
        out_msg->has_feature_flags = 1;
    }
    if (optional_json_integer(root, "property_id", 0, UINT8_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->property_id = (uint8_t)numeric;
        out_msg->has_property_id = 1;
    }
    if (optional_json_integer(root, "feature_value_int", INT32_MIN, INT32_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->feature_value_int = (int32_t)numeric;
        out_msg->has_feature_value_int = 1;
    }
    if (optional_json_integer(root, "feature_total", 0, UINT16_MAX,
                              &numeric, &present) != 0) goto cleanup;
    if (present) {
        out_msg->feature_total = (uint16_t)numeric;
        out_msg->has_feature_total = 1;
    }
    const cJSON *feature_bool_item =
        cJSON_GetObjectItemCaseSensitive(root, "feature_value_bool");
    if (feature_bool_item != NULL) {
        if (!cJSON_IsBool(feature_bool_item)) goto cleanup;
        out_msg->feature_value_bool = cJSON_IsTrue(feature_bool_item);
        out_msg->has_feature_value_bool = 1;
    }

    const cJSON *address_item = cJSON_GetObjectItemCaseSensitive(root, "ble_addr");
    if (address_item != NULL) {
        if (!cJSON_IsString(address_item) || address_item->valuestring == NULL ||
            parse_ble_addr(address_item->valuestring, out_msg->ble_addr) != 0) {
            goto cleanup;
        }
        const cJSON *address_type = cJSON_GetObjectItemCaseSensitive(root, "ble_addr_type");
        if (address_type != NULL) {
            if (!cJSON_IsNumber(address_type) || address_type->valueint < 0 ||
                address_type->valueint > UINT8_MAX ||
                address_type->valuedouble != (double)address_type->valueint) {
                goto cleanup;
            }
            out_msg->ble_addr_type = (uint8_t)address_type->valueint;
        }
        out_msg->has_ble_addr = 1;
    }

    result = 0;

cleanup:
    cJSON_Delete(root);
    if (result != 0) memset(out_msg, 0, sizeof(*out_msg));
    return result;
}
