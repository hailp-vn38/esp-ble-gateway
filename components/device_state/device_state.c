#include "device_state.h"

#include <string.h>

#include "command_executor.h"
#include "device_schema.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "gateway_events.h"

static const char *TAG = "device_state";

/* ── State storage ──────────────────────────────────────────────────── */

static device_state_entry_t s_entries[DEVICE_STATE_MAX_ENTRIES];
static size_t s_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;

static device_state_entry_t *find_entry(const char *device_id,
                                         const char *feature_id,
                                         uint8_t property_id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].device_id, device_id) == 0 &&
            strcmp(s_entries[i].feature_id, feature_id) == 0 &&
            s_entries[i].property_id == property_id) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static device_state_entry_t *allocate_entry(void)
{
    if (s_count < DEVICE_STATE_MAX_ENTRIES) {
        return &s_entries[s_count++];
    }
    return NULL;
}

/* ── State seed (commit listener) ───────────────────────────────────── */

static void seed_completion(const dispatch_result_t *result, void *context)
{
    (void)result;
    (void)context;
    /* Best-effort: ignore result. The cache still populates from
     * spontaneous feature_state events even if the seed command
     * is rejected by schema validation. */
}

static void on_schema_committed(const char *device_id, uint32_t revision,
                                 void *context)
{
    (void)revision;
    (void)context;

    device_schema_snapshot_t cap;
    if (device_schema_get(device_id, &cap) != ESP_OK || !cap.has_committed) {
        return;
    }

    ESP_LOGI(TAG, "[%s] seeding state for %zu features",
             device_id, cap.feature_count);

    for (size_t i = 0; i < cap.feature_count; i++) {
        const device_schema_feature_t *f = &cap.features[i];
        if (f->property_id == GW_PROP_NONE) {
            continue;
        }

        gw_message_t msg = {0};
        msg.protocol_version = GW_PROTOCOL_VERSION;
        strlcpy(msg.type, "device_command", sizeof(msg.type));
        strlcpy(msg.device_id, device_id, sizeof(msg.device_id));
        strlcpy(msg.command, "read_feature_state", sizeof(msg.command));
        msg.has_device_id = true;

        esp_err_t err = command_executor_submit(&msg, seed_completion, NULL);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "[%s] seed submit failed for %s: %s",
                     device_id, f->feature_id, esp_err_to_name(err));
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t device_state_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    s_initialized = true;

    esp_err_t err = device_schema_register_commit_listener2(
        on_schema_committed, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commit listener2 registration failed: %s",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "device_state initialized (max %d entries)",
             DEVICE_STATE_MAX_ENTRIES);
    return ESP_OK;
}

bool device_state_on_notify(const char *device_id, const gw_message_t *msg)
{
    if (msg == NULL || device_id == NULL) {
        return false;
    }

    if (strcmp(msg->type, "device_event") != 0 ||
        strcmp(msg->command, "feature_state") != 0 ||
        !msg->has_feature_id) {
        return false;
    }

    portENTER_CRITICAL(&s_lock);

    device_state_entry_t *entry = find_entry(device_id, msg->feature_id,
                                              msg->property_id);
    if (entry == NULL) {
        entry = allocate_entry();
        if (entry == NULL) {
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGW(TAG, "[%s] state table full, dropping %s",
                     device_id, msg->feature_id);
            return true;
        }
        strlcpy(entry->device_id, device_id, sizeof(entry->device_id));
        strlcpy(entry->feature_id, msg->feature_id,
                sizeof(entry->feature_id));
        entry->property_id = msg->property_id;
    }

    if (msg->has_feature_value_bool) {
        entry->value_bool = msg->feature_value_bool;
    }
    if (msg->has_feature_value_int) {
        entry->value_int = msg->feature_value_int;
    }
    entry->valid = true;
    entry->updated_at_ms = esp_timer_get_time() / 1000;

    /* Copy values for event before unlock */
    gateway_event_t ev = {0};
    ev.type = GW_EVENT_FEATURE_STATE;
    strlcpy(ev.device_id, device_id, sizeof(ev.device_id));
    strlcpy(ev.feature_id, msg->feature_id, sizeof(ev.feature_id));
    ev.property_id = msg->property_id;
    ev.updated_at_ms = entry->updated_at_ms;
    if (msg->has_feature_value_bool) {
        ev.value_kind = GW_EVENT_VALUE_BOOL;
        ev.bool_value = msg->feature_value_bool;
    } else if (msg->has_feature_value_int) {
        ev.value_kind = GW_EVENT_VALUE_INT;
        ev.int_value = msg->feature_value_int;
    }

    portEXIT_CRITICAL(&s_lock);

    gateway_events_publish(&ev);

    ESP_LOGI(TAG, "[%s] feature=%s prop=%u state updated",
             device_id, msg->feature_id, msg->property_id);

    return true;
}

esp_err_t device_state_get(const char *device_id,
                           const char *feature_id,
                           uint8_t property_id,
                           device_state_entry_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    const device_state_entry_t *entry = find_entry(device_id, feature_id,
                                                    property_id);
    bool valid = (entry != NULL && entry->valid);
    if (valid) {
        *out = *entry;
    }
    portEXIT_CRITICAL(&s_lock);

    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t device_state_snapshot(const char *device_id,
                                device_state_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out_snapshot->count = 0;

    portENTER_CRITICAL(&s_lock);

    for (size_t i = 0; i < s_count && out_snapshot->count < DEVICE_STATE_SNAPSHOT_MAX; i++) {
        if (strcmp(s_entries[i].device_id, device_id) == 0) {
            out_snapshot->entries[out_snapshot->count] = s_entries[i];
            out_snapshot->count++;
        }
    }

    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void device_state_forget(const char *device_id)
{
    if (device_id == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);

    size_t write = 0;
    for (size_t read = 0; read < s_count; read++) {
        if (strcmp(s_entries[read].device_id, device_id) != 0) {
            if (write != read) {
                s_entries[write] = s_entries[read];
            }
            write++;
        }
    }
    if (write < s_count) {
        ESP_LOGI(TAG, "[%s] forgot %zu state entries",
                 device_id, s_count - write);
    }
    s_count = write;

    portEXIT_CRITICAL(&s_lock);
}

void device_state_reset_for_test(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    portEXIT_CRITICAL(&s_lock);
}
