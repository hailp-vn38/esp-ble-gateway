#ifndef DEVICE_COMMAND_TYPES_H
#define DEVICE_COMMAND_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "cbor_codec.h"
#include "device_types.h"

typedef enum {
    DEVICE_CMD_ORIGIN_CONTROL = 0,
    DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY,
    DEVICE_CMD_ORIGIN_STATE_READ,
    DEVICE_CMD_ORIGIN_SETTINGS,
} device_command_origin_t;

/* Settings values have the same bounded representation as Protocol v4 CBOR. */
#define GW_SETTINGS_VALUE_STR_LEN GW_SETTINGS_VALUE_MAX_LEN

typedef struct {
    bool has_transaction_id;
    uint64_t transaction_id;
    bool has_expected_revision;
    uint32_t expected_revision;
    bool has_new_revision;
    uint32_t new_revision;
    bool has_setting_id;
    char setting_id[GW_FEATURE_ID_LEN];
    bool has_setting_value;
    uint8_t setting_type;
    union {
        bool bool_value;
        int32_t int_value;
        uint8_t enum_value;
        char string_value[GW_SETTINGS_VALUE_STR_LEN];
    } value;
} device_command_settings_payload_t;

typedef struct {
    device_command_origin_t origin;
    device_id_t device_id;
    device_command_t command;
    bool has_bool_value;
    bool bool_value;
    bool has_int_value;
    int32_t int_value;
    bool has_feature_id;
    device_feature_id_t feature_id;
    bool has_property_id;
    uint8_t property_id;
    device_command_settings_payload_t settings;
} device_command_request_t;

typedef enum {
    DEVICE_CMD_STATUS_OK = 0,
    DEVICE_CMD_STATUS_INVALID_ARGUMENT,
    DEVICE_CMD_STATUS_SCHEMA_NOT_READY,
    DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND,
    DEVICE_CMD_STATUS_TYPE_MISMATCH,
    DEVICE_CMD_STATUS_RANGE_ERROR,
    DEVICE_CMD_STATUS_NOT_CONNECTED,
    DEVICE_CMD_STATUS_BUSY,
    DEVICE_CMD_STATUS_QUEUE_FULL,
    DEVICE_CMD_STATUS_TRANSPORT_ERROR,
    DEVICE_CMD_STATUS_TIMEOUT,
    DEVICE_CMD_STATUS_REJECTED,
    DEVICE_CMD_STATUS_CANCELLED,
    DEVICE_CMD_STATUS_INTERNAL,
} device_command_status_t;

#define DEVICE_CMD_STATUS_DEVICE_REJECTED DEVICE_CMD_STATUS_REJECTED
#define DEVICE_CMD_STATUS_INTERNAL_ERROR DEVICE_CMD_STATUS_INTERNAL

typedef struct {
    device_command_status_t status;
    uint32_t request_id;
    bool accepted;
    bool has_bool_value;
    bool bool_value;
    bool has_int_value;
    int32_t int_value;
    bool has_feature_value_bool;
    bool feature_value_bool;
    bool has_feature_value_int;
    int32_t feature_value_int;
} device_command_result_t;

#endif /* DEVICE_COMMAND_TYPES_H */
