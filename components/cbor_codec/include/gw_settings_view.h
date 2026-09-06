#ifndef GW_SETTINGS_VIEW_H
#define GW_SETTINGS_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── CBOR key constants for Settings protocol ─────────────────────────
 * Extend the wire contract (Protocol v4 additive).  Keys 0–31 are
 * owned by the base protocol in cbor_codec.c.  Settings keys start at 32.
 *
 * These constants MUST match the device pack's gateway_protocol.h.
 * Do not renumber.  Non-intrusive: unknown keys from older gateways
 * are silently ignored by the targeted-lookup decoder pattern. ────── */

enum {
    GW_SETTINGS_CBOR_KEY_SETTINGS_BEGIN  = 32,
    GW_SETTINGS_CBOR_KEY_SETTINGS_ITEM   = 33,
    GW_SETTINGS_CBOR_KEY_SETTINGS_END    = 34,
    GW_SETTINGS_CBOR_KEY_SETTING_ID      = 35,
    GW_SETTINGS_CBOR_KEY_SETTING_TYPE    = 36,
    GW_SETTINGS_CBOR_KEY_WRITABLE        = 37,
    GW_SETTINGS_CBOR_KEY_ENUM_OPTIONS    = 38,
    GW_SETTINGS_CBOR_KEY_ENUM_VALUE      = 39,
    GW_SETTINGS_CBOR_KEY_ENUM_LABEL      = 40,
    GW_SETTINGS_CBOR_KEY_DEFAULT_VALUE   = 41,
    GW_SETTINGS_CBOR_KEY_MIN_VALUE       = 42,
    GW_SETTINGS_CBOR_KEY_MAX_VALUE       = 43,
    GW_SETTINGS_CBOR_KEY_STEP            = 44,
    GW_SETTINGS_CBOR_KEY_STRING_MAX_LEN  = 45,
    GW_SETTINGS_CBOR_KEY_CONFIG_REVISION = 46,
    GW_SETTINGS_CBOR_KEY_GROUP           = 47,
    GW_SETTINGS_CBOR_KEY_GROUP_LABEL     = 48,
    GW_SETTINGS_CBOR_KEY_GROUP_ORDER     = 49,
    GW_SETTINGS_CBOR_KEY_DEPENDENCY_ID   = 50,
    GW_SETTINGS_CBOR_KEY_DEPENDENCY_OP   = 51,
    GW_SETTINGS_CBOR_KEY_DEPENDENCY_VAL  = 52,
};

/* ── Settings value types (wire contract) ──────────────────────────── */

enum {
    GW_SETTINGS_TYPE_NONE   = 0,
    GW_SETTINGS_TYPE_BOOL   = 1,
    GW_SETTINGS_TYPE_INT    = 2,
    GW_SETTINGS_TYPE_FLOAT  = 3,
    GW_SETTINGS_TYPE_STRING = 4,
    GW_SETTINGS_TYPE_ENUM   = 5,
};

/* ── Message type strings for Settings protocol ────────────────────── */

#define GW_SETTINGS_MSG_SETTINGS_BEGIN       "settings_begin"
#define GW_SETTINGS_MSG_SETTINGS_ITEM        "settings_item"
#define GW_SETTINGS_MSG_SETTINGS_END         "settings_end"
#define GW_SETTINGS_MSG_SETTINGS_VALUES_BEGIN "settings_values_begin"
#define GW_SETTINGS_MSG_SETTINGS_VALUES_VALUE "settings_values_value"
#define GW_SETTINGS_MSG_SETTINGS_VALUES_END   "settings_values_end"

/* ── Command strings for Settings requests ─────────────────────────── */

#define GW_SETTINGS_CMD_DESCRIBE_SETTINGS  "describe_settings"
#define GW_SETTINGS_CMD_GET_SETTINGS       "get_settings"
#define GW_SETTINGS_CMD_SET_SETTINGS       "set_settings"
#define GW_SETTINGS_CMD_COMMIT_SETTINGS    "commit_settings"

/* ── Limits ────────────────────────────────────────────────────────── */

#define GW_SETTINGS_MAX_ENUM_OPTIONS  16
#define GW_SETTINGS_MAX_ID_LEN        32
#define GW_SETTINGS_MAX_GROUP_LEN     32
#define GW_SETTINGS_MAX_STRING_LEN    128
#define GW_SETTINGS_MAX_ENTRIES       12

/* ── Frame view (lifetime = raw BLE frame) ─────────────────────────── */

typedef struct {
    const uint8_t *buf;
    size_t len;
} gw_settings_frame_view_t;

/* ── Parsed settings entry (references into frame view) ────────────── */

typedef struct {
    const char *setting_id;
    uint8_t setting_type;
    bool writable;
    bool has_value;

    union {
        bool bool_val;
        int32_t int_val;
        float float_val;
        struct {
            const char *str;
            size_t max_len;
        } string_val;
        struct {
            int32_t value;
            const char *label;
        } enum_val;
    };

    struct {
        int32_t min_value;
        int32_t max_value;
        uint32_t step;
    } numeric_range;

    struct {
        uint8_t value;
        const char *label;
    } enum_options[GW_SETTINGS_MAX_ENUM_OPTIONS];
    uint8_t enum_option_count;

    const char *group;
    uint8_t group_order;
} gw_settings_entry_t;

/* ── Complete settings snapshot ────────────────────────────────────── */

typedef struct {
    uint32_t config_revision;
    uint16_t settings_count;
    gw_settings_entry_t entries[GW_SETTINGS_MAX_ENTRIES];
} gw_settings_snapshot_t;

/* ── Values entry (parsed from settings_values_value frame) ──────── */

typedef struct {
    const char *setting_id;
    uint8_t setting_type;
    bool has_value;

    union {
        bool bool_val;
        int32_t int_val;
        float float_val;
        struct {
            const char *str;
        } string_val;
        int32_t enum_val;
    };
} gw_settings_value_entry_t;

/* ── Values snapshot (built incrementally across frames) ─────────── */

typedef struct {
    uint32_t config_revision;
    uint16_t settings_count;
    gw_settings_value_entry_t entries[GW_SETTINGS_MAX_ENTRIES];
} gw_settings_values_snapshot_t;

/* ── API ────────────────────────────────────────────────────────────── */

/**
 * Parse a settings_item CBOR frame into a snapshot.
 *
 * All string pointers in the returned snapshot reference memory inside
 * the original frame buffer.  The caller MUST keep `view` alive while
 * using the snapshot.  For long-lived storage, deep-copy via PSRAM
 * allocator (G1).
 *
 * @param view   Pointer to raw CBOR frame.
 * @param len    Length of frame in bytes.
 * @param out    Output snapshot (zeroed on error).
 * @return 0 on success, -1 on parse/validation error.
 */
int gw_settings_frame_view_parse(const gw_settings_frame_view_t *view,
                                 size_t len,
                                 gw_settings_snapshot_t *out);

/**
 * Encode a settings request (describe_settings / get_settings /
 * set_settings / commit_settings) into a CBOR frame.
 *
 * @param command  One of the GW_SETTINGS_CMD_* strings.
 * @param device_id Device ID string.
 * @param out_buf  Output buffer.
 * @param out_buf_cap  Capacity of output buffer.
 * @return Encoded length > 0 on success, -1 on error.
 */
int gw_settings_encode_request(const char *command,
                               const char *device_id,
                               uint8_t *out_buf,
                               size_t out_buf_cap);

/**
 * Parse a settings_values_value CBOR frame into a value entry.
 *
 * All string pointers reference memory inside the original frame buffer.
 *
 * @param view   Pointer to raw CBOR frame.
 * @param len    Length of frame in bytes.
 * @param out    Output value entry (zeroed on error).
 * @return 0 on success, -1 on parse/validation error.
 */
int gw_settings_value_frame_view_parse(const gw_settings_frame_view_t *view,
                                       size_t len,
                                       gw_settings_value_entry_t *out);

#ifdef __cplusplus
}
#endif

#endif /* GW_SETTINGS_VIEW_H */
