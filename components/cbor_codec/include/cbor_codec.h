#ifndef CBOR_CODEC_H
#define CBOR_CODEC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define GW_MSG_MAX_LEN         256
#define GW_MSG_TYPE_LEN         24
#define GW_MSG_DEVICE_ID_LEN    32
#define GW_MSG_COMMAND_LEN      32
#define GW_MSG_NAME_LEN         32
#define GW_MSG_CAP_LABEL_LEN     32
#define GW_MSG_CAP_UNIT_LEN      12
#define GW_FEATURE_ID_LEN        32
#define GW_PROTOCOL_VERSION      4

/* Semantic feature types (wire contract v4, must match the Device's
 * gateway_protocol.h — do not renumber). */
typedef enum {
    GW_FEATURE_NONE = 0,
    GW_FEATURE_GENERIC_RELAY = 1,
    GW_FEATURE_ON_OFF_PLUGIN_UNIT = 10,
    GW_FEATURE_ON_OFF_LIGHT = 11,
    GW_FEATURE_DIMMABLE_LIGHT = 12,
    GW_FEATURE_FAN = 20,
    GW_FEATURE_TEMPERATURE_SENSOR = 30,
    GW_FEATURE_HUMIDITY_SENSOR = 31,
    GW_FEATURE_CONTACT_SENSOR = 40,
} gw_feature_type_t;

/* Semantic feature properties (wire contract v4, must match the Device's
 * gateway_protocol.h — do not renumber). */
typedef enum {
    GW_PROP_NONE = 0,
    GW_PROP_ON_OFF = 1,
    GW_PROP_LEVEL = 2,
    GW_PROP_PERCENT_SETTING = 3,
    GW_PROP_PERCENT_CURRENT = 4,
    GW_PROP_TEMPERATURE = 5,
    GW_PROP_HUMIDITY = 6,
    GW_PROP_CONTACT = 7,
} gw_feature_property_t;

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
    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    int has_ble_addr;

    // Capability discovery metadata (wire name kept from v3; the gateway
    // treats it as device_schema discovery transport). Optional and only
    // meaningful for capabilities_begin/capability_item/feature_item/
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

    // Protocol v4 semantic feature metadata/value fields.
    char feature_id[GW_FEATURE_ID_LEN];
    int has_feature_id;
    uint8_t feature_type;
    int has_feature_type;
    uint16_t feature_schema_version;
    int has_feature_schema_version;
    uint16_t feature_flags;
    int has_feature_flags;
    uint8_t property_id;
    int has_property_id;
    bool feature_value_bool;
    int has_feature_value_bool;
    int32_t feature_value_int;
    int has_feature_value_int;
    char feature_tool[GW_MSG_COMMAND_LEN];
    int has_feature_tool;
    uint16_t feature_total;
    int has_feature_total;
} gw_message_t;

int cbor_codec_decode(const uint8_t *buf, size_t len, gw_message_t *out_msg);
int cbor_codec_encode(const gw_message_t *msg, uint8_t *out_buf, size_t out_buf_cap);
int cbor_codec_msg_to_json(const gw_message_t *msg, char *out_json, size_t out_json_cap);
int cbor_codec_json_to_msg(const char *json_str, gw_message_t *out_msg);

#endif // CBOR_CODEC_H
