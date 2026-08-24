#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cbor_codec.h"

// PLACEHOLDER: binary layout don gian de build/test end-to-end ngay.
// San xuat thuc te: thay bang QCBOR/libcbor, dong bo layout voi thiet bi con.

int cbor_codec_decode(const uint8_t *buf, size_t len, gw_message_t *out_msg)
{
    if (buf == NULL || out_msg == NULL || len < 3) return -1;
    memset(out_msg, 0, sizeof(gw_message_t));

    size_t offset = 0;

    uint8_t type_len = buf[offset++];
    if (type_len >= GW_MSG_TYPE_LEN || offset + type_len > len) return -1;
    memcpy(out_msg->type, &buf[offset], type_len);
    out_msg->type[type_len] = '\0';
    offset += type_len;

    if (offset >= len) return -1;
    uint8_t id_len = buf[offset++];
    if (id_len >= GW_MSG_DEVICE_ID_LEN || offset + id_len > len) return -1;
    if (id_len > 0) {
        memcpy(out_msg->device_id, &buf[offset], id_len);
        out_msg->device_id[id_len] = '\0';
        out_msg->has_device_id = 1;
    }
    offset += id_len;

    if (offset >= len) return -1;
    uint8_t cmd_len = buf[offset++];
    if (cmd_len >= GW_MSG_COMMAND_LEN || offset + cmd_len + 5 > len) return -1;
    memcpy(out_msg->command, &buf[offset], cmd_len);
    out_msg->command[cmd_len] = '\0';
    offset += cmd_len;

    int32_t int_value;
    memcpy(&int_value, &buf[offset], sizeof(int32_t));
    out_msg->int_value = (int)int_value;
    offset += sizeof(int32_t);

    out_msg->bool_value = buf[offset];
    return 0;
}

int cbor_codec_encode(const gw_message_t *msg, uint8_t *out_buf, size_t out_buf_cap)
{
    if (msg == NULL || out_buf == NULL) return -1;

    size_t type_len = strnlen(msg->type, GW_MSG_TYPE_LEN - 1);
    size_t id_len = msg->has_device_id ? strnlen(msg->device_id, GW_MSG_DEVICE_ID_LEN - 1) : 0;
    size_t cmd_len = strnlen(msg->command, GW_MSG_COMMAND_LEN - 1);
    size_t total = 1 + type_len + 1 + id_len + 1 + cmd_len + sizeof(int32_t) + 1;

    if (total > out_buf_cap) return -1;

    size_t offset = 0;
    out_buf[offset++] = (uint8_t)type_len;
    memcpy(&out_buf[offset], msg->type, type_len);
    offset += type_len;

    out_buf[offset++] = (uint8_t)id_len;
    if (id_len > 0) {
        memcpy(&out_buf[offset], msg->device_id, id_len);
        offset += id_len;
    }

    out_buf[offset++] = (uint8_t)cmd_len;
    memcpy(&out_buf[offset], msg->command, cmd_len);
    offset += cmd_len;

    int32_t int_value = (int32_t)msg->int_value;
    memcpy(&out_buf[offset], &int_value, sizeof(int32_t));
    offset += sizeof(int32_t);

    out_buf[offset++] = (uint8_t)(msg->bool_value ? 1 : 0);
    return (int)offset;
}

int cbor_codec_msg_to_json(const gw_message_t *msg, char *out_json, size_t out_json_cap)
{
    if (msg == NULL || out_json == NULL) return -1;

    int written = snprintf(out_json, out_json_cap,
        "{\"type\":\"%s\",\"device_id\":\"%s\",\"command\":\"%s\","
        "\"int_value\":%d,\"bool_value\":%s}",
        msg->type, msg->has_device_id ? msg->device_id : "", msg->command,
        msg->int_value, msg->bool_value ? "true" : "false");

    if (written < 0 || (size_t)written >= out_json_cap) return -1;
    return written;
}

static int extract_json_string(const char *json, const char *key, char *out, size_t out_cap)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *pos = strstr(json, pattern);
    if (pos == NULL) return -1;
    pos += strlen(pattern);
    const char *end = strchr(pos, '"');
    if (end == NULL) return -1;
    size_t val_len = end - pos;
    if (val_len >= out_cap) val_len = out_cap - 1;
    memcpy(out, pos, val_len);
    out[val_len] = '\0';
    return 0;
}

static int extract_json_int(const char *json, const char *key, int *out)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = strstr(json, pattern);
    if (pos == NULL) return -1;
    *out = atoi(pos + strlen(pattern));
    return 0;
}

static int extract_json_bool(const char *json, const char *key, int *out)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = strstr(json, pattern);
    if (pos == NULL) return -1;
    pos += strlen(pattern);
    *out = (strncmp(pos, "true", 4) == 0) ? 1 : 0;
    return 0;
}

int cbor_codec_json_to_msg(const char *json_str, gw_message_t *out_msg)
{
    if (json_str == NULL || out_msg == NULL) return -1;
    memset(out_msg, 0, sizeof(gw_message_t));

    extract_json_string(json_str, "type", out_msg->type, sizeof(out_msg->type));
    extract_json_string(json_str, "command", out_msg->command, sizeof(out_msg->command));

    if (extract_json_string(json_str, "device_id", out_msg->device_id,
                             sizeof(out_msg->device_id)) == 0 && strlen(out_msg->device_id) > 0) {
        out_msg->has_device_id = 1;
    }

    extract_json_int(json_str, "int_value", &out_msg->int_value);
    extract_json_bool(json_str, "bool_value", &out_msg->bool_value);

    if (strlen(out_msg->type) == 0) return -1;
    return 0;
}
