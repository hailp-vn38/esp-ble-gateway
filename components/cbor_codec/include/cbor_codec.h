#ifndef CBOR_CODEC_H
#define CBOR_CODEC_H

#include <stdint.h>
#include <stddef.h>

#define GW_MSG_MAX_LEN         256
#define GW_MSG_TYPE_LEN         24
#define GW_MSG_DEVICE_ID_LEN    32
#define GW_MSG_COMMAND_LEN      32
#define GW_MSG_NAME_LEN         32
#define GW_MSG_DEVICE_TYPE_LEN  16
#define GW_MSG_CAP_LABEL_LEN     32
#define GW_MSG_CAP_UNIT_LEN      12
#define GW_PROTOCOL_VERSION      3

typedef struct {
    uint8_t protocol_version;
    char type[GW_MSG_TYPE_LEN];
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    uint32_t request_id;
    int has_request_id;
    int  int_value;
    int  bool_value;
    int  has_int_value;
    int  has_bool_value;
    int  has_device_id;
    char name[GW_MSG_NAME_LEN];
    char device_type[GW_MSG_DEVICE_TYPE_LEN];
    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    int has_ble_addr;

    // Protocol v3 capability discovery metadata. These fields are optional
    // and only meaningful for capabilities_begin/capability_item/
    // capabilities_end messages.
    uint32_t snapshot_id;
    int has_snapshot_id;
    uint16_t sequence;
    int has_sequence;
    uint16_t total;
    int has_total;
    uint8_t value_type;
    int has_value_type;
    uint8_t capability_flags;
    int has_capability_flags;
    int32_t min_value;
    int has_min_value;
    int32_t max_value;
    int has_max_value;
    uint32_t step;
    int has_step;
    char capability_label[GW_MSG_CAP_LABEL_LEN];
    char capability_unit[GW_MSG_CAP_UNIT_LEN];
    uint32_t capability_revision;
    int has_capability_revision;
} gw_message_t;

int cbor_codec_decode(const uint8_t *buf, size_t len, gw_message_t *out_msg);
int cbor_codec_encode(const gw_message_t *msg, uint8_t *out_buf, size_t out_buf_cap);
int cbor_codec_msg_to_json(const gw_message_t *msg, char *out_json, size_t out_json_cap);
int cbor_codec_json_to_msg(const char *json_str, gw_message_t *out_msg);

#endif // CBOR_CODEC_H
