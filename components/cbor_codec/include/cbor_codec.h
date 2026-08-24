#ifndef CBOR_CODEC_H
#define CBOR_CODEC_H

#include <stdint.h>
#include <stddef.h>

#define GW_MSG_MAX_LEN         256
#define GW_MSG_TYPE_LEN         24
#define GW_MSG_DEVICE_ID_LEN    32
#define GW_MSG_COMMAND_LEN      32

typedef struct {
    char type[GW_MSG_TYPE_LEN];
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    int  int_value;
    int  bool_value;
    int  has_device_id;
} gw_message_t;

int cbor_codec_decode(const uint8_t *buf, size_t len, gw_message_t *out_msg);
int cbor_codec_encode(const gw_message_t *msg, uint8_t *out_buf, size_t out_buf_cap);
int cbor_codec_msg_to_json(const gw_message_t *msg, char *out_json, size_t out_json_cap);
int cbor_codec_json_to_msg(const char *json_str, gw_message_t *out_msg);

#endif // CBOR_CODEC_H
