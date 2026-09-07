#ifndef GW_SETTINGS_VIEW_H
#define GW_SETTINGS_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Settings numeric keys, wire types, and flags are owned by cbor_codec.h.
 * This view provides parsed frame structures only; it must not renumber the
 * Protocol v4 contract. */

#define GW_SETTINGS_MSG_SETTINGS_BEGIN       "settings_begin"
#define GW_SETTINGS_MSG_SETTINGS_ITEM        "settings_item"
#define GW_SETTINGS_MSG_SETTINGS_END         "settings_end"
#define GW_SETTINGS_MSG_SETTINGS_VALUES_BEGIN "settings_values_begin"
#define GW_SETTINGS_MSG_SETTINGS_VALUES_VALUE "settings_values_value"
#define GW_SETTINGS_MSG_SETTINGS_VALUES_END   "settings_values_end"

#define GW_SETTINGS_CMD_DESCRIBE_SETTINGS  "describe_settings"
#define GW_SETTINGS_CMD_READ_SETTINGS      "read_settings"
#define GW_SETTINGS_CMD_GET_SETTINGS       "get_settings"
#define GW_SETTINGS_CMD_SET_SETTINGS       "set_settings"
#define GW_SETTINGS_CMD_COMMIT_SETTINGS    "commit_settings"

#define GW_SETTINGS_MAX_ENUM_OPTIONS  16
#define GW_SETTINGS_MAX_ID_LEN        32
#define GW_SETTINGS_MAX_GROUP_LEN     32
#define GW_SETTINGS_MAX_STRING_LEN    GW_SETTINGS_VALUE_MAX_LEN
#define GW_SETTINGS_MAX_ENTRIES       12
#define GW_SETTINGS_TEXT_SLOTS        (GW_SETTINGS_MAX_ENUM_OPTIONS + 4)

typedef struct {
    const uint8_t *buf;
    size_t len;
} gw_settings_frame_view_t;

typedef struct {
    bool has_sequence;
    uint16_t sequence;
    bool has_setting_id;
    char setting_id[GW_SETTINGS_MAX_ID_LEN];
    bool has_title;
    char title[GW_MSG_CAP_LABEL_LEN];
    bool has_group;
    char group[GW_SETTINGS_MAX_GROUP_LEN];
    bool has_unit;
    char unit[GW_MSG_CAP_UNIT_LEN];
    bool has_type;
    uint8_t type;
    bool has_flags;
    uint16_t flags;
    bool has_max_length;
    uint16_t max_length;
    bool has_option_index;
    uint8_t option_index;
    bool has_option_count;
    uint16_t option_count;
    bool has_transaction_id;
    uint64_t transaction_id;
    bool has_expected_revision;
    uint32_t expected_revision;
    bool has_new_revision;
    uint32_t new_revision;
    bool has_value;
    gw_settings_wire_value_t value;
} gw_settings_view_t;

/* Compatibility structures retained for code that consumes parsed snapshots. */
typedef struct {
    const char *setting_id;
    uint8_t setting_type;
    uint16_t flags;
    bool writable;
    bool has_value;
    union {
        bool bool_val;
        int32_t int_val;
        struct { const char *str; size_t max_len; } string_val;
        struct { int32_t value; const char *label; } enum_val;
    };
    struct { int32_t min_value; int32_t max_value; uint32_t step; } numeric_range;
    struct { uint8_t value; const char *label; } enum_options[GW_SETTINGS_MAX_ENUM_OPTIONS];
    uint8_t enum_option_count;
    const char *group;
} gw_settings_entry_t;

typedef struct {
    uint16_t settings_count;
    gw_settings_entry_t entries[GW_SETTINGS_MAX_ENTRIES];
    char text_storage[GW_SETTINGS_TEXT_SLOTS][GW_SETTINGS_MAX_STRING_LEN];
    gw_settings_view_t view;
} gw_settings_snapshot_t;

typedef struct {
    char setting_id_storage[GW_SETTINGS_MAX_ID_LEN];
    const char *setting_id;
    uint8_t setting_type;
    bool has_value;
    union {
        bool bool_val;
        int32_t int_val;
        struct { char storage[GW_SETTINGS_MAX_STRING_LEN]; const char *str; } string_val;
        int32_t enum_val;
    };
} gw_settings_value_entry_t;

typedef struct {
    uint32_t config_revision;
    uint16_t settings_count;
    gw_settings_value_entry_t entries[GW_SETTINGS_MAX_ENTRIES];
} gw_settings_values_snapshot_t;

int gw_settings_frame_view_parse(const gw_settings_frame_view_t *view,
                                 size_t len,
                                 gw_settings_snapshot_t *out);

int gw_settings_encode_request(const char *command,
                               const char *device_id,
                               uint8_t *out_buf,
                               size_t out_buf_cap);

int gw_settings_value_frame_view_parse(const gw_settings_frame_view_t *view,
                                       size_t len,
                                       gw_settings_value_entry_t *out);

#ifdef __cplusplus
}
#endif

#endif /* GW_SETTINGS_VIEW_H */
