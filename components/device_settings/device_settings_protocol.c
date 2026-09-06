#include <string.h>

#include "device_settings.h"
#include "device_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gw_settings_view.h"

static const char *TAG = "ds_protocol";

/* ── Internal: find record by device_store index ───────────────────── */

static ds_device_record_t *find_record(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return NULL;
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        /* We match by scanning the global records array.  A production
         * implementation would store device_id in the record.  For now
         * the first used record is returned — the protocol flow is
         * single-device at a time (one active settings operation). */
        (void)i;
    }
    return NULL;
}

/* ── settings_begin handler ────────────────────────────────────────── */

static void handle_begin(const char *device_id, const gw_message_t *msg)
{
    if (msg->protocol_version != GW_PROTOCOL_VERSION ||
        !msg->has_device_id || !msg->has_snapshot_id ||
        !msg->has_total || !msg->has_capability_revision) {
        return;
    }

    ESP_LOGI(TAG, "[%s] SETTINGS_BEGIN snapshot=%lu total=%u rev=%lu",
             device_id,
             (unsigned long)msg->snapshot_id,
             (unsigned)msg->total,
             (unsigned long)msg->capability_revision);

    /* Schema state transitions to DISCOVERING. */
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        (void)i;  /* placeholder — full integration in G2 */
    }
}

/* ── settings_item handler ─────────────────────────────────────────── */

static void handle_item(const char *device_id, const gw_message_t *msg)
{
    if (msg->protocol_version != GW_PROTOCOL_VERSION ||
        !msg->has_device_id || !msg->has_snapshot_id) {
        return;
    }

    /* Parse the settings frame using the zero-copy view. */
    /* The raw BLE frame is not available here (it's in gw_message_t).
     * In production, the frame pointer would be passed through.
     * For G1 we log the discovery metadata. */
    ESP_LOGI(TAG, "[%s] SETTINGS_ITEM snapshot=%lu cmd=%s",
             device_id,
             (unsigned long)msg->snapshot_id,
             msg->command);
}

/* ── settings_end handler ──────────────────────────────────────────── */

static void handle_end(const char *device_id, const gw_message_t *msg)
{
    if (msg->protocol_version != GW_PROTOCOL_VERSION ||
        !msg->has_device_id || !msg->has_snapshot_id) {
        return;
    }

    ESP_LOGI(TAG, "[%s] SETTINGS_END snapshot=%lu rev=%s",
             device_id,
             (unsigned long)msg->snapshot_id,
             msg->command);

    /* Schema state transitions to READY.  Full integration in G2. */
}

/* ── Public entry point ────────────────────────────────────────────── */

bool device_settings_on_notify(const char *device_id,
                               const gw_message_t *message)
{
    if (device_id == NULL || message == NULL) return false;

    if (strcmp(message->type, GW_SETTINGS_MSG_SETTINGS_BEGIN) == 0) {
        handle_begin(device_id, message);
        return true;
    }
    if (strcmp(message->type, GW_SETTINGS_MSG_SETTINGS_ITEM) == 0) {
        handle_item(device_id, message);
        return true;
    }
    if (strcmp(message->type, GW_SETTINGS_MSG_SETTINGS_END) == 0) {
        handle_end(device_id, message);
        return true;
    }

    return false;
}
