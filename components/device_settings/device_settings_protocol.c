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
static char s_schema_owner[GW_MSG_DEVICE_ID_LEN];
static uint32_t s_schema_request_id;
static uint8_t s_declared_option_count[DEVICE_SETTINGS_MAX_SETTINGS];

/* ── Values builder ─────────────────────────────────────────────────── */

static ds_values_builder_t s_values_builder;
static char s_values_owner[GW_MSG_DEVICE_ID_LEN];
static uint32_t s_values_request_id;
static uint32_t s_values_config_revision;

/* ── settings_begin handler ────────────────────────────────────────── */

static void schema_builder_discard(void)
{
    ds_schema_builder_reset(&s_schema_builder);
    s_schema_owner[0] = '\0';
    s_schema_request_id = 0;
    memset(s_declared_option_count, UINT8_MAX,
           sizeof(s_declared_option_count));
}

static void schema_reject(const char *device_id, ds_device_record_t *rec,
                          const char *reason)
{
    ESP_LOGW(TAG, "[REJECT reason=%s] device_id=%s request_id=%lu",
             reason, device_id, (unsigned long)s_schema_request_id);
    if (rec != NULL && rec->schema_stream_active &&
        strcmp(s_schema_owner, device_id) == 0) {
        rec->schema_stream_active = false;
        rec->schema_state = rec->schema != NULL ? DS_SCHEMA_READY
                                                : DS_SCHEMA_ERROR;
        rec->staging_expected_count = 0;
        rec->staging_received_count = 0;
        schema_builder_discard();
        DS_DIAG_INC(discovery_schema_fail);
    }
}

static bool schema_frame_matches(const char *device_id,
                                 const gw_message_t *msg,
                                 ds_device_record_t **out_rec)
{
    ds_device_record_t *rec = device_settings_find_record(device_id);
    *out_rec = rec;
    if (rec == NULL || !rec->schema_stream_active ||
        s_schema_owner[0] == '\0') {
        ESP_LOGW(TAG, "[REJECT reason=no_active_stream] device_id=%s",
                 device_id);
        return false;
    }
    if (strcmp(s_schema_owner, device_id) != 0) {
        ESP_LOGW(TAG, "[REJECT reason=wrong_device] device_id=%s owner=%s",
                 device_id, s_schema_owner);
        return false;
    }
    if (!msg->has_request_id || msg->request_id != s_schema_request_id) {
        schema_reject(device_id, rec, "request_id_mismatch");
        return false;
    }
    return true;
}

static uint16_t translate_wire_flags(uint16_t wire_flags)
{
    uint16_t flags = 0;
    if ((wire_flags & GW_SETTING_FLAG_READONLY) == 0) {
        flags |= DS_FLAG_WRITABLE;
    }
    if ((wire_flags & GW_SETTING_FLAG_SECRET) != 0) flags |= DS_FLAG_SECRET;
    if ((wire_flags & GW_SETTING_FLAG_ADVANCED) != 0) flags |= DS_FLAG_ADVANCED;
    return flags;
}

static void handle_begin(const char *device_id, const gw_message_t *msg)
{
    if (msg->protocol_version != GW_PROTOCOL_VERSION ||
        strcmp(msg->command, GW_SETTINGS_CMD_DESCRIBE_SETTINGS) != 0 ||
        !msg->has_request_id || !msg->has_total) {
        ESP_LOGW(TAG, "[REJECT reason=invalid_begin] device_id=%s", device_id);
        return;
    }
    if (msg->total > DEVICE_SETTINGS_MAX_SETTINGS) {
        ESP_LOGW(TAG, "[REJECT reason=total_exceeds_limit] device_id=%s total=%u",
                 device_id, (unsigned)msg->total);
        return;
    }
    if (s_schema_owner[0] != '\0') {
        ESP_LOGW(TAG, "[REJECT reason=builder_busy] device_id=%s owner=%s",
                 device_id, s_schema_owner);
        return;
    }

    ds_device_record_t *rec = device_settings_find_or_create_record(device_id);
    if (rec == NULL) {
        ESP_LOGE(TAG, "[REJECT reason=no_record_slot] device_id=%s", device_id);
        return;
    }

    if (ds_schema_builder_init(&s_schema_builder) != ESP_OK) {
        ESP_LOGE(TAG, "[REJECT reason=builder_alloc] device_id=%s", device_id);
        rec->schema_state = rec->schema != NULL ? DS_SCHEMA_READY
                                                : DS_SCHEMA_ERROR;
        DS_DIAG_INC(discovery_schema_fail);
        return;
    }
    s_schema_builder.schema_revision = msg->has_settings_schema_revision
                                            ? msg->settings_schema_revision
                                            : rec->advertised_schema_rev;
    strlcpy(s_schema_owner, device_id, sizeof(s_schema_owner));
    s_schema_request_id = msg->request_id;
    memset(s_declared_option_count, UINT8_MAX,
           sizeof(s_declared_option_count));

    rec->schema_state = DS_SCHEMA_DISCOVERING;
    rec->schema_stream_active = true;
    rec->staging_expected_count = msg->total;
    rec->staging_received_count = 0;

    ESP_LOGI(TAG, "[SCHEMA_BEGIN] device_id=%s request_id=%lu command=%s total=%u revision=%u",
             device_id, (unsigned long)msg->request_id, msg->command,
             (unsigned)msg->total,
             (unsigned)s_schema_builder.schema_revision);
}

/* ── settings_item handler ─────────────────────────────────────────── */

static void handle_item(const char *device_id, const gw_message_t *msg)
{
    ds_device_record_t *rec = NULL;
    if (!schema_frame_matches(device_id, msg, &rec)) return;
    if (!msg->has_total || !msg->has_settings_sequence ||
        !msg->has_setting_id || msg->setting_id[0] == '\0' ||
        !msg->has_setting_type || msg->setting_type < DS_TYPE_BOOL ||
        msg->setting_type > DS_TYPE_ENUM) {
        schema_reject(device_id, rec, "invalid_item");
        return;
    }
    if (msg->total != rec->staging_expected_count) {
        schema_reject(device_id, rec, "item_total_mismatch");
        return;
    }
    if (rec->staging_received_count >= rec->staging_expected_count) {
        schema_reject(device_id, rec, "item_count_overflow");
        return;
    }
    if (msg->settings_sequence != rec->staging_received_count) {
        schema_reject(device_id, rec,
                      msg->settings_sequence < rec->staging_received_count
                          ? "duplicate_sequence" : "sequence_gap");
        return;
    }

    /* Check for duplicate setting_id. */
    for (uint16_t i = 0; i < rec->staging_received_count; i++) {
        const char *existing_id = ds_string_pool_get(
            &s_schema_builder.strings,
            s_schema_builder.descriptors[i].id_off);
        if (existing_id != NULL &&
            strcmp(existing_id, msg->setting_id) == 0) {
            schema_reject(device_id, rec, "duplicate_setting_id");
            return;
        }
    }

    /* Add setting ID string to pool. */
    uint16_t id_off = 0;
    if (ds_schema_builder_add_string(&s_schema_builder, msg->setting_id,
                                     &id_off) != ESP_OK) {
        schema_reject(device_id, rec, "id_pool_exhausted");
        return;
    }

    uint16_t title_off = id_off;
    if (msg->setting_title[0] != '\0' &&
        ds_schema_builder_add_string(&s_schema_builder, msg->setting_title,
                                     &title_off) != ESP_OK) {
        schema_reject(device_id, rec, "title_pool_exhausted");
        return;
    }

    /* Add group string to pool (if present). */
    uint16_t group_off = 0;
    if (msg->has_setting_group && msg->setting_group[0] != '\0') {
        if (ds_schema_builder_add_string(&s_schema_builder,
                                         msg->setting_group,
                                         &group_off) != ESP_OK) {
            schema_reject(device_id, rec, "group_pool_exhausted");
            return;
        }
    }

    uint16_t unit_off = 0;
    if (msg->setting_unit[0] != '\0' &&
        ds_schema_builder_add_string(&s_schema_builder, msg->setting_unit,
                                     &unit_off) != ESP_OK) {
        schema_reject(device_id, rec, "unit_pool_exhausted");
        return;
    }

    /* Build setting descriptor. */
    ds_setting_desc_t desc = {
        .id_off = id_off,
        .title_off = title_off,
        .group_off = group_off,
        .unit_off = unit_off,
        .type = msg->setting_type,
        .flags = translate_wire_flags(msg->has_setting_flags
                                          ? msg->setting_flags : 0),
        .max_length = msg->has_settings_max_length
                          ? msg->settings_max_length : 0,
        .option_count = 0,
        .option_index = s_schema_builder.enum_option_count,
    };

    if (msg->has_min_value) desc.min_value = msg->min_value;
    if (msg->has_max_value) desc.max_value = msg->max_value;
    if (msg->has_step) desc.step = msg->step;

    if (msg->has_settings_option_count) {
        if (msg->settings_option_count > DEVICE_SETTINGS_MAX_ENUM_OPTS ||
            msg->setting_type != DS_TYPE_ENUM) {
            schema_reject(device_id, rec, "invalid_option_count");
            return;
        }
        s_declared_option_count[msg->settings_sequence] =
            (uint8_t)msg->settings_option_count;
    }

    if (ds_schema_builder_add_setting(&s_schema_builder, &desc) != ESP_OK) {
        schema_reject(device_id, rec, "descriptor_limit");
        return;
    }

    rec->staging_received_count++;
    ESP_LOGD(TAG, "[SCHEMA_ITEM] device_id=%s request_id=%lu sequence=%u/%u id=%s type=%u",
             device_id, (unsigned long)msg->request_id,
             (unsigned)msg->settings_sequence,
             (unsigned)rec->staging_expected_count, msg->setting_id,
             (unsigned)msg->setting_type);
    ESP_LOGD(TAG, "[SCHEMA_META] device_id=%s request_id=%lu sequence=%u flags=0x%04x max_length=%u",
             device_id, (unsigned long)msg->request_id,
             (unsigned)msg->settings_sequence, (unsigned)desc.flags,
             (unsigned)desc.max_length);
}

static void handle_option_item(const char *device_id, const gw_message_t *msg)
{
    ds_device_record_t *rec = NULL;
    if (!schema_frame_matches(device_id, msg, &rec)) return;
    if (!msg->has_settings_sequence || !msg->has_settings_option_index ||
        msg->setting_title[0] == '\0') {
        schema_reject(device_id, rec, "invalid_option");
        return;
    }
    if (msg->settings_sequence >= rec->staging_received_count) {
        schema_reject(device_id, rec, "option_before_parent");
        return;
    }

    ds_setting_desc_t *parent =
        &s_schema_builder.descriptors[msg->settings_sequence];
    if (parent->type != DS_TYPE_ENUM) {
        schema_reject(device_id, rec, "option_on_non_enum");
        return;
    }
    if (parent->option_count >= DEVICE_SETTINGS_MAX_ENUM_OPTS) {
        schema_reject(device_id, rec, "option_limit");
        return;
    }
    for (uint8_t i = 0; i < parent->option_count; i++) {
        if (s_schema_builder.enum_options[parent->option_index + i].value ==
            msg->settings_option_index) {
            schema_reject(device_id, rec, "duplicate_option_index");
            return;
        }
    }

    uint16_t option_slot = 0;
    if (ds_schema_builder_add_enum_option(&s_schema_builder,
                                          msg->settings_option_index,
                                          msg->setting_title,
                                          &option_slot) != ESP_OK) {
        schema_reject(device_id, rec, "option_pool_exhausted");
        return;
    }

    /* Keep each descriptor's options contiguous even if option frames for
     * different ENUM parents are interleaved on the wire. The builder API
     * appends, so insert the new entry at this parent's logical tail and
     * shift later option spans by one slot. */
    uint16_t insert_at = parent->option_count == 0
                             ? option_slot
                             : (uint16_t)(parent->option_index +
                                          parent->option_count);
    if (insert_at < option_slot) {
        ds_enum_option_t appended =
            s_schema_builder.enum_options[option_slot];
        memmove(&s_schema_builder.enum_options[insert_at + 1],
                &s_schema_builder.enum_options[insert_at],
                (option_slot - insert_at) * sizeof(ds_enum_option_t));
        s_schema_builder.enum_options[insert_at] = appended;
        for (uint16_t i = 0; i < s_schema_builder.setting_count; i++) {
            ds_setting_desc_t *other = &s_schema_builder.descriptors[i];
            if (other != parent && other->option_count > 0 &&
                other->option_index >= insert_at) {
                other->option_index++;
            }
        }
    }
    if (parent->option_count == 0) parent->option_index = insert_at;
    parent->option_count++;
    ESP_LOGD(TAG, "[SCHEMA_OPTION] device_id=%s request_id=%lu sequence=%u option=%u",
             device_id, (unsigned long)msg->request_id,
             (unsigned)msg->settings_sequence,
             (unsigned)msg->settings_option_index);
}

/* ── settings_end handler ──────────────────────────────────────────── */

static void handle_end(const char *device_id, const gw_message_t *msg)
{
    ds_device_record_t *rec = NULL;
    if (!schema_frame_matches(device_id, msg, &rec)) return;
    if (!msg->has_total || msg->total != rec->staging_expected_count ||
        rec->staging_received_count != rec->staging_expected_count) {
        schema_reject(device_id, rec, "end_total_mismatch");
        return;
    }
    for (uint16_t i = 0; i < s_schema_builder.setting_count; i++) {
        if (s_declared_option_count[i] != UINT8_MAX &&
            s_declared_option_count[i] !=
                s_schema_builder.descriptors[i].option_count) {
            schema_reject(device_id, rec, "option_count_mismatch");
            return;
        }
    }

    ESP_LOGI(TAG, "[SCHEMA_END] device_id=%s request_id=%lu total=%u",
             device_id, (unsigned long)msg->request_id,
             (unsigned)msg->total);

    /* Commit schema builder → PSRAM snapshot. */
    ds_schema_t *schema = ds_schema_builder_commit(&s_schema_builder);
    if (schema == NULL) {
        schema_reject(device_id, rec, "commit_alloc");
        return;
    }

    if (device_settings_commit_schema(device_id, schema) != ESP_OK) {
        ds_settings_ref_release(schema);
        schema_reject(device_id, rec, "commit_swap");
        return;
    }
    rec->schema_stream_active = false;
    DS_DIAG_INC(discovery_schema_success);

    ESP_LOGI(TAG, "[SCHEMA_COMMIT] device_id=%s request_id=%lu count=%u revision=%lu",
             device_id, (unsigned long)msg->request_id,
             (unsigned)schema->setting_count,
             (unsigned long)schema->schema_revision);
    schema_builder_discard();

    /* READ is queued by the worker after DESCRIBE ACK completes.
     * No auto-GET here — the worker owns the operation lifecycle. */
}

void device_settings_protocol_on_disconnect(const char *device_id)
{
    if (s_schema_owner[0] != '\0' &&
        (device_id == NULL || strcmp(device_id, s_schema_owner) == 0)) {
        schema_builder_discard();
    }
    if (s_values_owner[0] != '\0' &&
        (device_id == NULL || strcmp(device_id, s_values_owner) == 0)) {
        ds_values_builder_reset(&s_values_builder);
        s_values_owner[0] = '\0';
        s_values_request_id = 0;
        s_values_config_revision = 0;
    }
}

/* ── settings_values_begin handler ─────────────────────────────────── */

static void values_builder_discard(void)
{
    ds_values_builder_reset(&s_values_builder);
    s_values_owner[0] = '\0';
    s_values_request_id = 0;
    s_values_config_revision = 0;
}

static void values_reject(const char *device_id, ds_device_record_t *rec,
                          const char *reason)
{
    ESP_LOGW(TAG, "[VALUES_REJECT reason=%s] device_id=%s request_id=%lu",
             reason, device_id, (unsigned long)s_values_request_id);
    if (rec != NULL && rec->values_stream_active &&
        strcmp(s_values_owner, device_id) == 0) {
        rec->values_stream_active = false;
        rec->staging_expected_count = 0;
        rec->staging_received_count = 0;
        values_builder_discard();
        DS_DIAG_INC(discovery_values_fail);
    }
}

static bool values_frame_matches(const char *device_id,
                                 const gw_message_t *msg,
                                 ds_device_record_t **out_rec)
{
    ds_device_record_t *rec = device_settings_find_record(device_id);
    *out_rec = rec;
    if (rec == NULL || !rec->values_stream_active ||
        s_values_owner[0] == '\0') {
        ESP_LOGW(TAG, "[VALUES_REJECT reason=no_active_stream] device_id=%s",
                 device_id);
        return false;
    }
    if (strcmp(s_values_owner, device_id) != 0) {
        ESP_LOGW(TAG, "[VALUES_REJECT reason=wrong_device] device_id=%s owner=%s",
                 device_id, s_values_owner);
        return false;
    }
    if (!msg->has_request_id || msg->request_id != s_values_request_id) {
        values_reject(device_id, rec, "request_id_mismatch");
        return false;
    }
    return true;
}

static const ds_setting_desc_t *find_descriptor(const ds_schema_t *schema,
                                                 const char *setting_id)
{
    if (schema == NULL || setting_id == NULL) return NULL;
    for (uint16_t i = 0; i < schema->setting_count; i++) {
        const ds_setting_desc_t *desc = &schema->descriptors[i];
        if (strcmp(ds_string_pool_get(&schema->strings, desc->id_off),
                   setting_id) == 0) {
            return desc;
        }
    }
    return NULL;
}

static bool enum_value_allowed(const ds_schema_t *schema,
                               const ds_setting_desc_t *desc, uint8_t value)
{
    for (uint8_t i = 0; i < desc->option_count; i++) {
        if (schema->enum_option_pool[desc->option_index + i].value == value) {
            return true;
        }
    }
    return false;
}

static void handle_values_begin(const char *device_id,
                                const gw_message_t *msg)
{
    if (msg->protocol_version != GW_PROTOCOL_VERSION || !msg->has_request_id ||
        !msg->has_total || !msg->has_capability_revision) {
        ESP_LOGW(TAG, "[VALUES_REJECT reason=invalid_begin] device_id=%s",
                 device_id);
        return;
    }

    if (s_values_owner[0] != '\0') {
        ESP_LOGW(TAG, "[VALUES_REJECT reason=builder_busy] device_id=%s owner=%s",
                 device_id, s_values_owner);
        return;
    }

    ds_device_record_t *rec = device_settings_find_record(device_id);
    if (rec == NULL) return;

    /* Must have committed schema first. */
    if (rec->schema_state != DS_SCHEMA_READY || rec->schema == NULL) {
        ESP_LOGW(TAG, "[VALUES_REJECT reason=schema_not_ready] device_id=%s",
                 device_id);
        return;
    }

    if (msg->total > rec->schema->setting_count) {
        ESP_LOGW(TAG, "[VALUES_REJECT reason=total_exceeds_schema] device_id=%s total=%u schema_count=%u",
                 device_id,
                 (unsigned)msg->total,
                 (unsigned)rec->schema->setting_count);
        return;
    }

    /* Initialize values builder. */
    if (ds_values_builder_init(&s_values_builder) != ESP_OK) {
        ESP_LOGE(TAG, "[VALUES_REJECT reason=builder_alloc] device_id=%s",
                 device_id);
        DS_DIAG_INC(discovery_values_fail);
        return;
    }
    s_values_builder.config_revision = msg->capability_revision;
    strlcpy(s_values_owner, device_id, sizeof(s_values_owner));
    s_values_request_id = msg->request_id;
    s_values_config_revision = msg->capability_revision;

    rec->values_stream_active = true;
    rec->staging_expected_count = msg->total;
    rec->staging_received_count = 0;
    rec->staging_snapshot_id = 0;
    ESP_LOGI(TAG, "[VALUES_BEGIN] device_id=%s request_id=%lu total=%u config_rev=%lu",
             device_id, (unsigned long)msg->request_id, (unsigned)msg->total,
             (unsigned long)msg->capability_revision);
}

/* ── settings_values_value handler ─────────────────────────────────── */

static void handle_values_value(const char *device_id,
                                const gw_message_t *msg)
{
    ds_device_record_t *rec = NULL;
    if (!values_frame_matches(device_id, msg, &rec)) return;
    if (!msg->has_settings_sequence || !msg->has_setting_id ||
        msg->setting_id[0] == '\0' || !msg->has_setting_type ||
        !msg->has_setting_value) {
        values_reject(device_id, rec, "invalid_value");
        return;
    }

    /* Validate count. */
    if (rec->staging_received_count >= rec->staging_expected_count) {
        values_reject(device_id, rec, "count_overflow");
        return;
    }
    if (msg->settings_sequence != rec->staging_received_count) {
        values_reject(device_id, rec,
                      msg->settings_sequence < rec->staging_received_count
                          ? "duplicate_sequence" : "sequence_gap");
        return;
    }

    const ds_setting_desc_t *desc = find_descriptor(rec->schema, msg->setting_id);
    if (desc == NULL) {
        values_reject(device_id, rec, "unknown_setting_id");
        return;
    }
    if (desc->type != msg->setting_type) {
        values_reject(device_id, rec, "type_mismatch");
        return;
    }

    ds_value_entry_t entry = {
        .id_off = desc->id_off,
        .type = msg->setting_type,
        .has_value = (desc->flags & DS_FLAG_SECRET) == 0,
    };

    switch (msg->setting_type) {
    case DS_TYPE_BOOL:
        entry.bool_val = msg->setting_value.setting_value_bool;
        break;
    case DS_TYPE_INT:
        entry.int_val = msg->setting_value.setting_value_int;
        break;
    case DS_TYPE_STRING:
        if (strlen(msg->setting_value.setting_value_string) >
            GW_SETTINGS_VALUE_MAX_LEN - 1 ||
            (desc->max_length != 0 &&
             strlen(msg->setting_value.setting_value_string) > desc->max_length)) {
            values_reject(device_id, rec, "string_too_long");
            return;
        }
        if (entry.has_value && ds_string_pool_add(
                &s_values_builder.string_pool,
                msg->setting_value.setting_value_string,
                &entry.string_off) != ESP_OK) {
            values_reject(device_id, rec, "string_pool_exhausted");
            return;
        }
        break;
    case DS_TYPE_ENUM:
        entry.enum_val = msg->setting_value.setting_value_enum;
        if (!enum_value_allowed(rec->schema, desc,
                                msg->setting_value.setting_value_enum)) {
            values_reject(device_id, rec, "enum_out_of_range");
            return;
        }
        break;
    default:
        values_reject(device_id, rec, "unsupported_type");
        return;
    }

    /* A secret frame may be structurally valid, but its raw value must not
     * survive the receive callback. Keep only the descriptor identity and
     * an unavailable marker in the committed snapshot. */
    if (desc->flags & DS_FLAG_SECRET) {
        entry.has_value = false;
        entry.bool_val = false; /* clears the value union without logging it */
    }

    if (ds_values_builder_add(&s_values_builder, &entry) != ESP_OK) {
        values_reject(device_id, rec, "builder_add");
        return;
    }

    rec->staging_received_count++;
    if (desc->flags & DS_FLAG_SECRET) {
        ESP_LOGD(TAG, "[VALUE] device_id=%s request_id=%lu sequence=%u/%u id=%s value=<redacted>",
                 device_id, (unsigned long)msg->request_id,
                 (unsigned)msg->settings_sequence,
                 (unsigned)rec->staging_expected_count, msg->setting_id);
    } else if (msg->setting_type == DS_TYPE_STRING) {
        ESP_LOGD(TAG, "[VALUE] device_id=%s request_id=%lu sequence=%u/%u id=%s STRING len=%u",
                 device_id, (unsigned long)msg->request_id,
                 (unsigned)msg->settings_sequence,
                 (unsigned)rec->staging_expected_count, msg->setting_id,
                 (unsigned)strlen(msg->setting_value.setting_value_string));
    } else {
        ESP_LOGD(TAG, "[VALUE] device_id=%s request_id=%lu sequence=%u/%u id=%s type=%u",
                 device_id, (unsigned long)msg->request_id,
                 (unsigned)msg->settings_sequence,
                 (unsigned)rec->staging_expected_count, msg->setting_id,
                 (unsigned)msg->setting_type);
    }
}

/* ── settings_values_end handler ───────────────────────────────────── */

static void handle_values_end(const char *device_id,
                              const gw_message_t *msg)
{
    ds_device_record_t *rec = NULL;
    if (!values_frame_matches(device_id, msg, &rec)) return;
    if (!msg->has_total || !msg->has_capability_revision) {
        values_reject(device_id, rec, "invalid_end");
        return;
    }

    /* Validate count. */
    if (msg->total != rec->staging_expected_count ||
        rec->staging_received_count != rec->staging_expected_count) {
        values_reject(device_id, rec, "end_total_mismatch");
        return;
    }
    if (msg->capability_revision != s_values_config_revision) {
        values_reject(device_id, rec, "revision_mismatch");
        return;
    }

    ESP_LOGI(TAG, "[VALUES_END] device_id=%s request_id=%lu total=%u config_rev=%lu",
             device_id, (unsigned long)msg->request_id, (unsigned)msg->total,
             (unsigned long)msg->capability_revision);

    /* Commit values builder → PSRAM snapshot. */
    ds_values_t *values = ds_values_builder_commit(&s_values_builder);
    if (values == NULL) {
        values_reject(device_id, rec, "commit_alloc");
        return;
    }

    if (device_settings_commit_values(device_id, values) != ESP_OK) {
        ds_values_ref_release(values);
        values_reject(device_id, rec, "commit_swap");
        return;
    }
    rec->values_stream_active = false;
    DS_DIAG_INC(discovery_values_success);

    ESP_LOGI(TAG, "[VALUES_COMMIT] device_id=%s request_id=%lu count=%u config_rev=%lu",
             device_id, (unsigned long)msg->request_id,
             (unsigned)values->value_count,
             (unsigned long)values->config_revision);
    values_builder_discard();

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
    if (strcmp(message->type, GW_SETTINGS_MSG_SETTINGS_OPTION_ITEM) == 0) {
        handle_option_item(device_id, message);
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
