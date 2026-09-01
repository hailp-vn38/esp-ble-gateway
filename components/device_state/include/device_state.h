#ifndef DEVICE_STATE_H
#define DEVICE_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "cbor_codec.h"
#include "esp_err.h"

#define DEVICE_STATE_MAX_ENTRIES 96
#define DEVICE_STATE_SNAPSHOT_MAX 12  /* matches DEVICE_SCHEMA_MAX_FEATURES */

/* ── State entry ────────────────────────────────────────────────────── */

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char feature_id[GW_FEATURE_ID_LEN];
    uint8_t property_id;
    bool value_bool;
    int32_t value_int;
    bool valid;
    int64_t updated_at_ms;  /* esp_timer_get_time() / 1000 */
} device_state_entry_t;

/* ── Snapshot (copy-out, safe across mutations) ──────────────────────── */

typedef struct {
    device_state_entry_t entries[DEVICE_STATE_SNAPSHOT_MAX];
    size_t count;
} device_state_snapshot_t;

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t device_state_init(void);

/**
 * Handle a BLE notification. Returns true if consumed (type=device_event,
 * command=feature_state). Does NOT forward to dispatcher.
 */
bool device_state_on_notify(const char *device_id, const gw_message_t *msg);

/**
 * Get current state for a specific (device_id, feature_id, property_id).
 * Returns ESP_ERR_NOT_FOUND if no entry exists.
 */
esp_err_t device_state_get(const char *device_id,
                           const char *feature_id,
                           uint8_t property_id,
                           device_state_entry_t *out);

/**
 * Get all state entries for a device (copy-out snapshot).
 * Entries are copied into out_snapshot->entries; safe across mutations.
 * Count may be 0.
 */
esp_err_t device_state_snapshot(const char *device_id,
                                device_state_snapshot_t *out_snapshot);

/** Clear all state entries for a device (called on disconnect). */
void device_state_forget(const char *device_id);

/** Reset all state (test only). */
void device_state_reset_for_test(void);

#endif /* DEVICE_STATE_H */
