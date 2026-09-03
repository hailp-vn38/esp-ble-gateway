#include "device_schema_internal.h"

#include <stddef.h>
#include <string.h>

#include "device_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gateway_events.h"

static const char *TAG = "schema_proto";

/* ── Wire message helpers ───────────────────────────────────────────── */

static bool message_device_matches(const char *device_id,
                                   const gw_message_t *message)
{
    return message->protocol_version == GW_PROTOCOL_VERSION &&
           message->has_device_id &&
           strcmp(device_id, message->device_id) == 0;
}

static bool snapshot_content_equal(const device_schema_snapshot_t *a,
                                   const device_schema_snapshot_t *b)
{
    if (a->revision != b->revision ||
        a->tool_count != b->tool_count ||
        a->feature_count != b->feature_count) {
        return false;
    }
    for (size_t i = 0; i < a->tool_count; i++) {
        if (!schema_tool_equal(&a->tools[i], &b->tools[i])) return false;
    }
    for (size_t i = 0; i < a->feature_count; i++) {
        if (memcmp(&a->features[i], &b->features[i],
                   sizeof(device_schema_feature_t)) != 0) {
            return false;
        }
    }
    return true;
}

/* ── BEGIN handler ──────────────────────────────────────────────────── */

static void handle_begin(const char *device_id, const gw_message_t *message)
{
    if (!message_device_matches(device_id, message) ||
        !message->has_snapshot_id || !message->has_total ||
        !message->has_capability_revision ||
        message->total > DEVICE_SCHEMA_MAX_TOOLS ||
        (message->has_feature_total &&
         message->feature_total > DEVICE_SCHEMA_MAX_FEATURES)) {
        return;
    }
    if (!schema_runtime_lock()) return;
    schema_record_t *record = schema_runtime_find_locked(device_id);
    if (record != NULL) {
        if (record->operation_state != SCHEMA_OP_RUNNING) {
            schema_runtime_unlock();
            return;
        }
        memset(&record->staging, 0, sizeof(record->staging));
        strlcpy(record->staging.device_id, device_id,
                sizeof(record->staging.device_id));
        record->staging.state = DEVICE_SCHEMA_STATE_DISCOVERING;
        record->staging.snapshot_id = message->snapshot_id;
        record->staging.revision = message->capability_revision;
        record->staging_expected_tools = message->total;
        record->staging_expected_features =
            message->has_feature_total ? message->feature_total : 0;
        record->staging_active = true;
        ESP_LOGI(TAG, "[%s] SCHEMA_BEGIN snapshot=%lu tools=%u features=%u rev=%lu",
                 device_id, (unsigned long)message->snapshot_id,
                 (unsigned)message->total,
                 (unsigned)record->staging_expected_features,
                 (unsigned long)message->capability_revision);
    }
    schema_runtime_unlock();
}

/* ── TOOL_ITEM handler ──────────────────────────────────────────────── */

static void handle_tool_item(const char *device_id,
                              const gw_message_t *message)
{
    if (!message_device_matches(device_id, message) ||
        !message->has_snapshot_id || !message->has_sequence ||
        !message->has_value_type) {
        return;
    }

    device_schema_tool_t tool = {
        .value_type = message->value_type,
        .flags = message->has_capability_flags
                     ? message->capability_flags
                     : 0,
        .min_value = message->min_value,
        .max_value = message->max_value,
        .step = message->step,
    };
    strlcpy(tool.command, message->command, sizeof(tool.command));
    strlcpy(tool.label,
            message->capability_label[0] != '\0'
                ? message->capability_label
                : message->command,
            sizeof(tool.label));
    strlcpy(tool.unit, message->capability_unit, sizeof(tool.unit));

    if (tool.value_type == 2 /* INT */ &&
        (!message->has_min_value || !message->has_max_value ||
         !message->has_step)) {
        return;
    }
    if (!schema_valid_tool(&tool)) return;

    if (!schema_runtime_lock()) return;
    schema_record_t *record = schema_runtime_find_locked(device_id);
    if (record != NULL) {
        size_t next = record->staging.tool_count;
        bool valid_sequence =
            record->staging_active &&
            record->staging.snapshot_id == message->snapshot_id &&
            message->sequence == next && next < record->staging_expected_tools;
        if (valid_sequence) {
            for (size_t i = 0; i < next; i++) {
                if (strcmp(record->staging.tools[i].command,
                           tool.command) == 0) {
                    valid_sequence = false;
                    break;
                }
            }
        }
        if (valid_sequence) {
            record->staging.tools[next] = tool;
            record->staging.tool_count++;
        } else {
            ESP_LOGW(TAG, "[%s] SCHEMA_TOOL_ITEM invalid seq=%u", device_id,
                     (unsigned)message->sequence);
            record->staging_active = false;
            memset(&record->staging, 0, sizeof(record->staging));
            if (!record->has_committed) {
                record->committed.state = DEVICE_SCHEMA_STATE_ERROR;
            }
            record->operation_state = SCHEMA_OP_IDLE;
        }
    }
    schema_runtime_unlock();
}

/* ── FEATURE_ITEM handler ───────────────────────────────────────────── */

static void handle_feature_item(const char *device_id,
                                 const gw_message_t *message)
{
    if (!message_device_matches(device_id, message) ||
        !message->has_snapshot_id || !message->has_feature_id ||
        !message->has_feature_type || !message->has_property_id) {
        return;
    }

    if (!schema_valid_feature_id(message->feature_id)) return;

    device_schema_feature_t feature = {
        .feature_type = message->feature_type,
        .feature_schema_version = message->has_feature_schema_version
                                      ? message->feature_schema_version
                                      : 0,
        .feature_flags = message->has_feature_flags
                             ? message->feature_flags
                             : 0,
        .property_id = message->property_id,
        .feature_value_bool = message->has_feature_value_bool
                                  ? message->feature_value_bool
                                  : false,
        .feature_value_int = message->has_feature_value_int
                                 ? message->feature_value_int
                                 : 0,
        .writable_tool_index = -1,
    };
    strlcpy(feature.feature_id, message->feature_id,
            sizeof(feature.feature_id));

    /* Resolve writable tool index if feature_tool is provided. */
    if (!schema_runtime_lock()) return;
    schema_record_t *record = schema_runtime_find_locked(device_id);
    if (record != NULL) {
        size_t next = record->staging.feature_count;
        bool valid_sequence =
            record->staging_active &&
            record->staging.snapshot_id == message->snapshot_id &&
            message->sequence == (record->staging_expected_tools + next) &&
            next < record->staging_expected_features;
        if (valid_sequence) {
            /* Check duplicate feature_id. */
            for (size_t i = 0; i < next; i++) {
                if (strcmp(record->staging.features[i].feature_id,
                           feature.feature_id) == 0) {
                    valid_sequence = false;
                    break;
                }
            }
        }
        if (valid_sequence) {
            /* Resolve writable tool. */
            if (message->has_feature_tool && message->feature_tool[0] != '\0') {
                feature.writable_tool_index = schema_resolve_writable_tool(
                    record->staging.tools, record->staging.tool_count,
                    message->feature_tool);
                if (feature.writable_tool_index < 0) {
                    ESP_LOGW(TAG, "[%s] SCHEMA_FEATURE_ITEM tool '%s' not found",
                             device_id, message->feature_tool);
                    valid_sequence = false;
                }
            }
        }
        if (valid_sequence) {
            record->staging.features[next] = feature;
            record->staging.feature_count++;
        } else {
            ESP_LOGW(TAG, "[%s] SCHEMA_FEATURE_ITEM invalid", device_id);
            record->staging_active = false;
            memset(&record->staging, 0, sizeof(record->staging));
            if (!record->has_committed) {
                record->committed.state = DEVICE_SCHEMA_STATE_ERROR;
            }
            record->operation_state = SCHEMA_OP_IDLE;
        }
    }
    schema_runtime_unlock();
}

/* ── END handler ────────────────────────────────────────────────────── */

static void handle_end(const char *device_id, const gw_message_t *message)
{
    if (!message_device_matches(device_id, message) ||
        !message->has_snapshot_id || !message->has_total) {
        return;
    }

    device_schema_snapshot_t committed;
    bool persist = false;
    bool changed = false;
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (!schema_runtime_lock()) return;
    schema_record_t *record = schema_runtime_find_locked(device_id);
    if (record != NULL) {
        bool complete =
            record->staging_active &&
            record->staging.snapshot_id == message->snapshot_id &&
            message->total == record->staging_expected_tools &&
            record->staging.tool_count == record->staging_expected_tools &&
            record->staging.feature_count == record->staging_expected_features;
        if (complete) {
            changed = !record->has_committed ||
                      !snapshot_content_equal(&record->committed,
                                              &record->staging);
            if (record->has_committed &&
                record->committed.revision == record->staging.revision &&
                changed) {
                ESP_LOGW(TAG,
                         "[%s] SCHEMA_REVISION_MISMATCH revision=%lu "
                         "content_changed=true",
                         device_id, (unsigned long)record->committed.revision);
            }
            record->staging.state = DEVICE_SCHEMA_STATE_READY;
            record->staging.updated_at_ms = now_ms;
            record->committed = record->staging;
            record->has_committed = true;
            record->staging_active = false;
            committed = record->committed;
            persist = true;

            ESP_LOGI(TAG,
                     "[%s] SCHEMA_END committed %u tools %u features "
                     "(revision=%lu changed=%d)",
                     device_id, (unsigned)committed.tool_count,
                     (unsigned)committed.feature_count,
                     (unsigned long)committed.revision, (int)changed);
        } else {
            ESP_LOGW(TAG, "[%s] SCHEMA_END incomplete", device_id);
            record->staging_active = false;
            memset(&record->staging, 0, sizeof(record->staging));
            if (!record->has_committed) {
                record->committed.state = DEVICE_SCHEMA_STATE_ERROR;
            }
            record->operation_state = SCHEMA_OP_IDLE;
        }
    }
    schema_runtime_unlock();

    if (persist) {
        /* Find index for NVS persist. */
        int persist_index = -1;
        if (schema_runtime_lock()) {
            schema_record_t *r = schema_runtime_find_locked(device_id);
            if (r != NULL) {
                schema_record_t *records = schema_runtime_records();
                for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
                    if (&records[i] == r) {
                        persist_index = i;
                        break;
                    }
                }
            }
            schema_runtime_unlock();
        }

        if (!changed) {
            if (schema_runtime_lock()) {
                schema_record_t *r = schema_runtime_find_locked(device_id);
                if (r != NULL && r->persist_dirty) {
                    esp_err_t err = schema_persist_record(persist_index,
                                                          &committed);
                    if (err == ESP_OK) {
                        r->persist_dirty = false;
                    }
                }
                schema_runtime_unlock();
            }
        } else {
            esp_err_t error = schema_persist_record(persist_index, &committed);
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "[%s] NVS persist failed: %s (persist_dirty=true)",
                         device_id, esp_err_to_name(error));
                if (schema_runtime_lock()) {
                    schema_record_t *r = schema_runtime_find_locked(device_id);
                    if (r != NULL) {
                        r->persist_dirty = true;
                    }
                    schema_runtime_unlock();
                }
            } else {
                ESP_LOGI(TAG, "[%s] persisted schema to NVS", device_id);
            }
        }
    }

    /* Commit listener fires outside the schema mutex, after the persist
     * decision, for every successful commit (changed or not). It must only
     * enqueue work — the exposure consumer relies on this contract. */
    if (persist) {
        schema_runtime_notify_commit(device_id, committed.revision);
    }

    /* Publish schema event for realtime consumers (WebSocket, etc.) */
    if (persist) {
        gateway_event_t ev = {0};
        ev.type = GW_EVENT_DEVICE_SCHEMA;
        strlcpy(ev.device_id, device_id, sizeof(ev.device_id));
        ev.schema_revision = committed.revision;
        gateway_events_publish(&ev);
    }
}

/* ── Public entry point ─────────────────────────────────────────────── */

void schema_protocol_handle_message(const char *device_id,
                                    const gw_message_t *message)
{
    if (strcmp(message->type, "capabilities_begin") == 0) {
        handle_begin(device_id, message);
    } else if (strcmp(message->type, "capability_item") == 0) {
        handle_tool_item(device_id, message);
    } else if (strcmp(message->type, "feature_item") == 0) {
        handle_feature_item(device_id, message);
    } else {
        handle_end(device_id, message);
    }
}
