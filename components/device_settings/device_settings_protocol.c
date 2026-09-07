#include <string.h>

#include "device_settings.h"
#include "device_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gateway_events.h"
#include "gw_settings_view.h"

static const char *TAG = "ds_protocol";

/* ── Schema builder (stack-allocated during stream) ───────────────────
 * The builder accumulates descriptors in internal SRAM during the
 * settings_begin → settings_item → settings_end stream.  On commit
 * it allocates the final ds_schema_t in PSRAM and transfers string
 * pool ownership. */

static ds_schema_builder_t s_schema_builder;

/* ── Values builder ─────────────────────────────────────────────────── */

static ds_values_builder_t s_values_builder;

/* ── settings_begin handler ────────────────────────────────────────── */

static void handle_begin(const char *device_id, const gw_message_t *msg)
{
    if (!msg->has_device_id || !msg->has_snapshot_id ||
        !msg->has_total || !msg->has_settings_schema_revision) {
        return;
    }

    if (msg->total > DEVICE_SETTINGS_MAX_SETTINGS) {
        ESP_LOGW(TAG, "[%s] BEGIN total=%u exceeds max %d, rejecting",
                 device_id, (unsigned)msg->total,
                 DEVICE_SETTINGS_MAX_SETTINGS);
        return;
    }

    ESP_LOGI(TAG, "[%s] SETTINGS_BEGIN snapshot=%lu total=%u rev=%lu",
             device_id,
             (unsigned long)msg->snapshot_id,
             (unsigned)msg->total,
             (unsigned long)msg->capability_revision);

    ds_device_record_t *rec = device_settings_find_or_create_record(device_id);
    if (rec == NULL) {
        ESP_LOGE(TAG, "[%s] BEGIN no free record slot", device_id);
        return;
    }

    /* Initialize schema builder. */
    ds_schema_builder_reset(&s_schema_builder);
    if (ds_schema_builder_init(&s_schema_builder) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] BEGIN schema builder init failed (PSRAM?)",
                 device_id);
        rec->schema_state = DS_SCHEMA_ERROR;
        return;
    }
    s_schema_builder.schema_revision = msg->settings_schema_revision;

    rec->schema_state = DS_SCHEMA_DISCOVERING;
    rec->schema_stream_active = true;
    rec->staging_snapshot_id = msg->snapshot_id;
    rec->staging_expected_count = msg->total;
    rec->staging_received_count = 0;
}

/* ── settings_item handler ─────────────────────────────────────────── */

static void handle_item(const char *device_id, const gw_message_t *msg)
{
    if (!msg->has_device_id || !msg->has_snapshot_id ||
        !msg->has_setting_id || !msg->has_setting_type) {
        return;
    }

    /* Validate setting type is within range. */
    if (msg->setting_type > DS_TYPE_ENUM) {
        ESP_LOGW(TAG, "[%s] ITEM invalid type=%u, rejecting",
                 device_id, (unsigned)msg->setting_type);
        return;
    }

    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec == NULL || !rec->schema_stream_active) {
        return;
    }

    /* Validate snapshot matches active stream. */
    if (msg->snapshot_id != rec->staging_snapshot_id) {
        ESP_LOGW(TAG, "[%s] ITEM snapshot mismatch (got %lu, expected %lu)",
                 device_id,
                 (unsigned long)msg->snapshot_id,
                 (unsigned long)rec->staging_snapshot_id);
        return;
    }

    /* Validate count. */
    if (rec->staging_received_count >= rec->staging_expected_count) {
        ESP_LOGW(TAG, "[%s] ITEM count overflow (%u >= %u)",
                 device_id,
                 (unsigned)rec->staging_received_count,
                 (unsigned)rec->staging_expected_count);
        rec->schema_stream_active = false;
        rec->schema_state = DS_SCHEMA_ERROR;
        DS_DIAG_INC(discovery_schema_fail);
        return;
    }

    /* Check for duplicate setting_id. */
    for (uint16_t i = 0; i < rec->staging_received_count; i++) {
        const char *existing_id = ds_string_pool_get(
            &s_schema_builder.strings,
            s_schema_builder.descriptors[i].id_off);
        if (existing_id != NULL &&
            strcmp(existing_id, msg->setting_id) == 0) {
            ESP_LOGW(TAG, "[%s] ITEM duplicate id='%s'",
                     device_id, msg->setting_id);
            rec->schema_stream_active = false;
            rec->schema_state = DS_SCHEMA_ERROR;
            DS_DIAG_INC(discovery_schema_fail);
            return;
        }
    }

    /* Add setting ID string to pool. */
    uint16_t id_off = 0;
    if (ds_schema_builder_add_string(&s_schema_builder, msg->setting_id,
                                     &id_off) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] ITEM string pool exhausted for id", device_id);
        rec->schema_stream_active = false;
        rec->schema_state = DS_SCHEMA_ERROR;
        DS_DIAG_INC(discovery_schema_fail);
        return;
    }

    /* Add group string to pool (if present). */
    uint16_t group_off = 0;
    if (msg->has_setting_group && msg->setting_group[0] != '\0') {
        if (ds_schema_builder_add_string(&s_schema_builder,
                                         msg->setting_group,
                                         &group_off) != ESP_OK) {
            ESP_LOGE(TAG, "[%s] ITEM string pool exhausted for group",
                     device_id);
            rec->schema_stream_active = false;
            rec->schema_state = DS_SCHEMA_ERROR;
            DS_DIAG_INC(discovery_schema_fail);
            return;
        }
    }

    /* Build setting descriptor. */
    ds_setting_desc_t desc = {
        .id_off = id_off,
        .title_off = id_off,  /* title defaults to id */
        .group_off = group_off,
        .unit_off = 0,
        .type = msg->setting_type,
        .flags = msg->has_setting_flags ? msg->setting_flags : 0,
        .option_count = 0,
        .option_index = 0,
    };

    if (msg->has_min_value) desc.min_value = msg->min_value;
    if (msg->has_max_value) desc.max_value = msg->max_value;
    if (msg->has_step) desc.step = (int32_t)msg->step;

    if (ds_schema_builder_add_setting(&s_schema_builder, &desc) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] ITEM builder add failed", device_id);
        rec->schema_stream_active = false;
        rec->schema_state = DS_SCHEMA_ERROR;
        DS_DIAG_INC(discovery_schema_fail);
        return;
    }

    rec->staging_received_count++;
    ESP_LOGD(TAG, "[%s] ITEM %u/%u id='%s' type=%u",
             device_id,
             (unsigned)rec->staging_received_count,
             (unsigned)rec->staging_expected_count,
             msg->setting_id,
             (unsigned)msg->setting_type);
}

/* ── settings_end handler ──────────────────────────────────────────── */

static void handle_end(const char *device_id, const gw_message_t *msg)
{
    if (!msg->has_device_id || !msg->has_snapshot_id || !msg->has_total) {
        return;
    }

    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec == NULL || !rec->schema_stream_active) {
        return;
    }

    /* Validate snapshot and count. */
    if (msg->snapshot_id != rec->staging_snapshot_id ||
        msg->total != rec->staging_expected_count ||
        rec->staging_received_count != rec->staging_expected_count) {
        ESP_LOGW(TAG, "[%s] END mismatch: snapshot=%lu/%lu total=%u/%u "
                 "received=%u",
                 device_id,
                 (unsigned long)msg->snapshot_id,
                 (unsigned long)rec->staging_snapshot_id,
                 (unsigned)msg->total,
                 (unsigned)rec->staging_expected_count,
                 (unsigned)rec->staging_received_count);
        rec->schema_stream_active = false;
        rec->schema_state = DS_SCHEMA_ERROR;
        DS_DIAG_INC(discovery_schema_fail);
        return;
    }

    /* Commit schema builder → PSRAM snapshot. */
    ds_schema_t *schema = ds_schema_builder_commit(&s_schema_builder);
    if (schema == NULL) {
        ESP_LOGE(TAG, "[%s] END commit failed (PSRAM?)", device_id);
        rec->schema_stream_active = false;
        rec->schema_state = DS_SCHEMA_ERROR;
        DS_DIAG_INC(discovery_schema_fail);
        return;
    }

    /* Atomic swap: free old, install new. */
    if (rec->schema != NULL) {
        ds_settings_ref_release(rec->schema);
    }
    rec->schema = schema;
    rec->schema_rev = schema->schema_revision;
    rec->schema_state = DS_SCHEMA_READY;
    rec->schema_stream_active = false;
    DS_DIAG_INC(discovery_schema_success);

    ESP_LOGI(TAG, "[%s] SCHEMA committed: %u settings, revision=%lu",
             device_id,
             (unsigned)schema->setting_count,
             (unsigned long)schema->schema_revision);

    /* After schema commit, automatically trigger values read. */
    esp_err_t err = device_settings_get(device_id, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] auto GET queued failed: %s",
                 device_id, esp_err_to_name(err));
    }
}

/* ── settings_values_begin handler ─────────────────────────────────── */

static void handle_values_begin(const char *device_id,
                                const gw_message_t *msg)
{
    if (!msg->has_device_id || !msg->has_snapshot_id ||
        !msg->has_total || !msg->has_capability_revision) {
        return;
    }

    ESP_LOGI(TAG, "[%s] VALUES_BEGIN snapshot=%lu total=%u config_rev=%lu",
             device_id,
             (unsigned long)msg->snapshot_id,
             (unsigned)msg->total,
             (unsigned long)msg->capability_revision);

    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec == NULL) return;

    /* Must have committed schema first. */
    if (rec->schema_state != DS_SCHEMA_READY || rec->schema == NULL) {
        ESP_LOGW(TAG, "[%s] VALUES_BEGIN but schema not ready", device_id);
        return;
    }

    if (msg->total > rec->schema->setting_count) {
        ESP_LOGW(TAG, "[%s] VALUES_BEGIN total=%u exceeds schema count=%u",
                 device_id,
                 (unsigned)msg->total,
                 (unsigned)rec->schema->setting_count);
        return;
    }

    /* Initialize values builder. */
    ds_values_builder_reset(&s_values_builder);
    if (ds_values_builder_init(&s_values_builder) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] VALUES_BEGIN builder init failed (PSRAM?)",
                 device_id);
        return;
    }
    s_values_builder.config_revision = msg->capability_revision;

    rec->values_stream_active = true;
    rec->staging_expected_count = msg->total;
    rec->staging_received_count = 0;
    rec->staging_snapshot_id = msg->snapshot_id;
}

/* ── settings_values_value handler ─────────────────────────────────── */

static void handle_values_value(const char *device_id,
                                const gw_message_t *msg)
{
    if (!msg->has_device_id || !msg->has_snapshot_id ||
        !msg->has_setting_id || !msg->has_setting_type) {
        return;
    }

    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec == NULL || !rec->values_stream_active) {
        return;
    }

    /* Validate snapshot. */
    if (msg->snapshot_id != rec->staging_snapshot_id) {
        ESP_LOGW(TAG, "[%s] VALUES_VALUE snapshot mismatch", device_id);
        return;
    }

    /* Validate count. */
    if (rec->staging_received_count >= rec->staging_expected_count) {
        ESP_LOGW(TAG, "[%s] VALUES_VALUE count overflow", device_id);
        rec->values_stream_active = false;
        return;
    }

    /* Validate setting_id exists in committed schema and type matches. */
    bool found = false;
    uint16_t id_off = 0;
    for (uint16_t i = 0; i < rec->schema->setting_count; i++) {
        const char *schema_id = ds_string_pool_get(
            &rec->schema->strings,
            rec->schema->descriptors[i].id_off);
        if (schema_id != NULL && strcmp(schema_id, msg->setting_id) == 0) {
            if (rec->schema->descriptors[i].type != msg->setting_type) {
                ESP_LOGW(TAG, "[%s] VALUES_VALUE type mismatch: schema=%u "
                         "value=%u for '%s'",
                         device_id,
                         (unsigned)rec->schema->descriptors[i].type,
                         (unsigned)msg->setting_type,
                         msg->setting_id);
                rec->values_stream_active = false;
                return;
            }
            id_off = rec->schema->descriptors[i].id_off;
            found = true;
            break;
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "[%s] VALUES_VALUE id='%s' not in schema",
                 device_id, msg->setting_id);
        rec->values_stream_active = false;
        return;
    }

    /* Build value entry. */
    ds_value_entry_t entry = {
        .id_off = id_off,
        .type = msg->setting_type,
        .has_value = true,
    };

    if (!msg->has_setting_value) {
        ESP_LOGW(TAG, "[%s] VALUES_VALUE missing typed value", device_id);
        rec->values_stream_active = false;
        return;
    }

    switch (msg->setting_type) {
    case DS_TYPE_BOOL:
        entry.bool_val = msg->setting_value.setting_value_bool;
        break;
    case DS_TYPE_INT:
        entry.int_val = msg->setting_value.setting_value_int;
        break;
    case DS_TYPE_STRING:
        entry.string_off = 0;
        break;
    case DS_TYPE_ENUM:
        entry.enum_val = msg->setting_value.setting_value_enum;
        break;
    default:
        entry.has_value = false;
        break;
    }

    if (ds_values_builder_add(&s_values_builder, &entry) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] VALUES_VALUE builder add failed", device_id);
        rec->values_stream_active = false;
        return;
    }

    rec->staging_received_count++;
    ESP_LOGD(TAG, "[%s] VALUES_VALUE %u/%u id='%s'",
             device_id,
             (unsigned)rec->staging_received_count,
             (unsigned)rec->staging_expected_count,
             msg->setting_id);
}

/* ── settings_values_end handler ───────────────────────────────────── */

static void handle_values_end(const char *device_id,
                              const gw_message_t *msg)
{
    if (!msg->has_device_id || !msg->has_snapshot_id || !msg->has_total) {
        return;
    }

    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec == NULL || !rec->values_stream_active) {
        return;
    }

    /* Validate count. */
    if (msg->total != rec->staging_expected_count ||
        rec->staging_received_count != rec->staging_expected_count) {
        ESP_LOGW(TAG, "[%s] VALUES_END mismatch: total=%u expected=%u "
                 "received=%u",
                 device_id,
                 (unsigned)msg->total,
                 (unsigned)rec->staging_expected_count,
                 (unsigned)rec->staging_received_count);
        rec->values_stream_active = false;
        DS_DIAG_INC(discovery_values_fail);
        return;
    }

    /* Commit values builder → PSRAM snapshot. */
    ds_values_t *values = ds_values_builder_commit(&s_values_builder);
    if (values == NULL) {
        ESP_LOGE(TAG, "[%s] VALUES_END commit failed (PSRAM?)", device_id);
        rec->values_stream_active = false;
        DS_DIAG_INC(discovery_values_fail);
        return;
    }

    /* Atomic swap: free old, install new. */
    if (rec->values != NULL) {
        ds_values_ref_release(rec->values);
    }
    rec->values = values;
    rec->config_rev = values->config_revision;
    rec->values_stream_active = false;
    DS_DIAG_INC(discovery_values_success);

    ESP_LOGI(TAG, "[%s] VALUES committed: %u values, config_rev=%lu",
             device_id,
             (unsigned)values->value_count,
             (unsigned long)values->config_revision);

    /* Publish settings changed event. */
    {
        gateway_event_t ev = {
            .type = GW_EVENT_SETTINGS_CHANGED,
            .config_revision = values->config_revision,
        };
        strlcpy(ev.device_id, device_id, sizeof(ev.device_id));
        gateway_events_publish(&ev);
    }

    /* If a reconciliation is pending (post-COMMIT reboot), verify now. */
    if (rec->pending_reconciliation) {
        device_settings_reconcile(device_id);
    }
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
    if (strcmp(message->type, GW_SETTINGS_MSG_SETTINGS_VALUES_BEGIN) == 0) {
        handle_values_begin(device_id, message);
        return true;
    }
    if (strcmp(message->type, GW_SETTINGS_MSG_SETTINGS_VALUES_VALUE) == 0) {
        handle_values_value(device_id, message);
        return true;
    }
    if (strcmp(message->type, GW_SETTINGS_MSG_SETTINGS_VALUES_END) == 0) {
        handle_values_end(device_id, message);
        return true;
    }

    return false;
}
