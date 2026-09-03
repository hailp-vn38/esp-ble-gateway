#include "device_schema.h"
#include "device_schema_internal.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "device_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gateway_events.h"
#include "memory_policy.h"

static const char *TAG = "device_schema";

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t device_schema_init(void)
{
    if (schema_runtime_is_initialized()) return ESP_ERR_INVALID_STATE;

    esp_err_t err = schema_runtime_init();
    if (err != ESP_OK) return err;

    err = schema_worker_init();
    if (err != ESP_OK) return err;

    schema_runtime_set_initialized(true);
    return ESP_OK;
}

void device_schema_set_submitter(device_schema_submit_fn submitter)
{
    schema_runtime_set_submitter(submitter);
}

esp_err_t device_schema_register_commit_listener(
    device_schema_commit_listener_t listener, void *context)
{
    if (!schema_runtime_lock()) return ESP_ERR_TIMEOUT;
    schema_runtime_set_commit_listener(listener, context);
    schema_runtime_unlock();
    return ESP_OK;
}

esp_err_t device_schema_register_commit_listener2(
    device_schema_commit_listener2_t listener, void *context)
{
    if (!schema_runtime_lock()) return ESP_ERR_TIMEOUT;
    schema_runtime_set_commit_listener2(listener, context);
    schema_runtime_unlock();
    return ESP_OK;
}

esp_err_t device_schema_on_ready(const char *device_id)
{
    if (!schema_runtime_is_initialized()) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || device_id[0] == '\0' ||
        strnlen(device_id, GW_MSG_DEVICE_ID_LEN) >= GW_MSG_DEVICE_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    schema_event_t event = {
        .type = SCHEMA_EVENT_READY,
    };
    strlcpy(event.device_id, device_id, sizeof(event.device_id));
    return schema_worker_post_event(&event);
}

void device_schema_on_disconnect(const char *device_id)
{
    if (!schema_runtime_is_initialized()) return;
    if (device_id == NULL || device_id[0] == '\0') return;
    schema_event_t event = {
        .type = SCHEMA_EVENT_DISCONNECT,
    };
    strlcpy(event.device_id, device_id, sizeof(event.device_id));
    schema_worker_post_event(&event);
}

bool device_schema_on_notify(const char *device_id,
                              const gw_message_t *message)
{
    return schema_worker_post_notify(device_id, message);
}

void device_schema_get_queue_stats(device_schema_queue_stats_t *out)
{
    schema_worker_get_stats(out);
}

esp_err_t device_schema_refresh(const char *device_id,
                                 uint32_t *out_generation)
{
    if (device_id == NULL || device_id[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!schema_runtime_is_initialized() ||
        schema_runtime_records() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!schema_runtime_lock()) return ESP_ERR_TIMEOUT;
    schema_record_t *record =
        schema_runtime_find_or_create_locked(device_id);
    if (record == NULL) {
        schema_runtime_unlock();
        return ESP_ERR_NO_MEM;
    }

    if (record->operation_state == SCHEMA_OP_QUEUED ||
        record->operation_state == SCHEMA_OP_RUNNING) {
        schema_runtime_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t gen = schema_runtime_next_generation();
    uint32_t op_id = schema_runtime_next_operation_id();

    record->operation_kind = SCHEMA_OP_MANUAL;
    record->operation_state = SCHEMA_OP_QUEUED;
    record->operation_id = op_id;
    record->refresh_active.generation = gen;
    record->refresh_active.state = DEVICE_SCHEMA_REFRESH_QUEUED;

    if (out_generation != NULL) *out_generation = gen;

    ESP_LOGI(TAG, "[%s] SCHEMA_REFRESH_RESERVE gen=%lu op_id=%lu",
             device_id, (unsigned long)gen, (unsigned long)op_id);

    schema_runtime_unlock();

    schema_event_t event = {
        .type = SCHEMA_EVENT_REFRESH,
        .operation_id = op_id,
        .refresh_generation = gen,
    };
    strlcpy(event.device_id, device_id, sizeof(event.device_id));
    esp_err_t err = schema_worker_post_event(&event);
    if (err != ESP_OK) {
        if (schema_runtime_lock()) {
            schema_record_t *r = schema_runtime_find_locked(device_id);
            if (r != NULL) {
                r->operation_state = SCHEMA_OP_IDLE;
                r->refresh_active.state = DEVICE_SCHEMA_REFRESH_IDLE;
                r->refresh_active.generation = 0;
            }
            schema_runtime_unlock();
        }
        return err;
    }

    return ESP_OK;
}

esp_err_t device_schema_get(const char *device_id,
                             device_schema_snapshot_t *out_snapshot)
{
    if (device_id == NULL || out_snapshot == NULL) return ESP_ERR_INVALID_ARG;
    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    strlcpy(out_snapshot->device_id, device_id,
            sizeof(out_snapshot->device_id));
    out_snapshot->state = DEVICE_SCHEMA_STATE_UNKNOWN;
    if (!schema_runtime_lock()) return ESP_ERR_TIMEOUT;
    schema_record_t *record = schema_runtime_find_locked(device_id);
    if (record != NULL) {
        *out_snapshot = record->committed;
        out_snapshot->has_committed = record->has_committed;
    }
    schema_runtime_unlock();
    return ESP_OK;
}

esp_err_t device_schema_get_refresh_status(
    const char *device_id,
    device_schema_refresh_active_t *out_active,
    device_schema_refresh_completed_t *out_completed)
{
    if (device_id == NULL) return ESP_ERR_INVALID_ARG;
    if (!schema_runtime_lock()) return ESP_ERR_TIMEOUT;
    schema_record_t *record = schema_runtime_find_locked(device_id);
    if (record == NULL) {
        schema_runtime_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (out_active != NULL) {
        *out_active = record->refresh_active;
    }
    if (out_completed != NULL) {
        *out_completed = record->refresh_last_completed;
    }
    schema_runtime_unlock();
    return ESP_OK;
}

device_schema_validation_t device_schema_validate_command(
    const gw_message_t *message, device_schema_tool_t *out_tool)
{
    if (message == NULL || !message->has_device_id ||
        message->command[0] == '\0') {
        return DEVICE_SCHEMA_VALID_ARGUMENT;
    }
    if (strcmp(message->command, DEVICE_SCHEMA_RESERVED_COMMAND) == 0) {
        return DEVICE_SCHEMA_VALID;
    }
    if (!schema_runtime_lock()) return DEVICE_SCHEMA_VALID_UNKNOWN;
    schema_record_t *record = schema_runtime_find_locked(message->device_id);
    if (record == NULL || !record->has_committed ||
        (record->committed.state != DEVICE_SCHEMA_STATE_READY &&
         record->committed.state != DEVICE_SCHEMA_STATE_DISCOVERING)) {
        schema_runtime_unlock();
        return DEVICE_SCHEMA_VALID_UNKNOWN;
    }

    const device_schema_tool_t *tool = NULL;
    for (size_t i = 0; i < record->committed.tool_count; i++) {
        if (strcmp(record->committed.tools[i].command,
                   message->command) == 0) {
            tool = &record->committed.tools[i];
            break;
        }
    }
    if (tool == NULL) {
        schema_runtime_unlock();
        return DEVICE_SCHEMA_VALID_UNSUPPORTED_COMMAND;
    }

    bool valid_argument = false;
    switch (tool->value_type) {
    case 0: /* NONE */
        valid_argument = !message->has_int_value && !message->has_bool_value;
        break;
    case 1: /* BOOL */
        valid_argument = message->has_bool_value && !message->has_int_value;
        break;
    case 2: { /* INT */
        int64_t delta = (int64_t)message->int_value -
                        (int64_t)tool->min_value;
        valid_argument = message->has_int_value && !message->has_bool_value &&
                         message->int_value >= tool->min_value &&
                         message->int_value <= tool->max_value &&
                         ((uint64_t)delta % tool->step == 0);
        break;
    }
    }
    if (valid_argument && out_tool != NULL) {
        *out_tool = *tool;
    }
    schema_runtime_unlock();
    return valid_argument ? DEVICE_SCHEMA_VALID : DEVICE_SCHEMA_VALID_ARGUMENT;
}

esp_err_t device_schema_forget(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return ESP_ERR_INVALID_ARG;

    if (!schema_runtime_lock()) return ESP_ERR_TIMEOUT;
    schema_record_t *record = schema_runtime_find_locked(device_id);
    if (record == NULL) {
        schema_runtime_unlock();
        return ESP_OK;
    }

    int forget_index = -1;
    schema_record_t *records = schema_runtime_records();
    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (&records[i] == record) {
            forget_index = i;
            break;
        }
    }

    if (record->operation_state == SCHEMA_OP_QUEUED ||
        record->operation_state == SCHEMA_OP_RUNNING) {
        if (schema_runtime_owner()->active &&
            strcmp(schema_runtime_owner()->device_id, device_id) == 0 &&
            schema_runtime_owner()->operation_id == record->operation_id) {
            schema_runtime_owner()->active = false;
        }
        record->operation_state = SCHEMA_OP_IDLE;
    }
    record->staging_active = false;
    memset(&record->staging, 0, sizeof(record->staging));
    schema_runtime_unlock();

    esp_err_t error = schema_erase_nvs(forget_index);

    if (error != ESP_OK) {
        ESP_LOGW(TAG, "[%s] NVS erase failed: %s, RAM cache preserved",
                 device_id, esp_err_to_name(error));
        return error;
    }

    if (schema_runtime_lock()) {
        schema_record_t *r = schema_runtime_find_locked(device_id);
        if (r != NULL) {
            memset(r, 0, sizeof(*r));
        }
        schema_runtime_unlock();
    }
    return ESP_OK;
}

const char *device_schema_state_name(device_schema_state_t state)
{
    switch (state) {
    case DEVICE_SCHEMA_STATE_UNKNOWN: return "unknown";
    case DEVICE_SCHEMA_STATE_DISCOVERING: return "discovering";
    case DEVICE_SCHEMA_STATE_READY: return "ready";
    case DEVICE_SCHEMA_STATE_UNSUPPORTED: return "unsupported";
    case DEVICE_SCHEMA_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

const char *device_schema_refresh_result_name(
    device_schema_refresh_result_t result)
{
    switch (result) {
    case DEVICE_SCHEMA_REFRESH_RESULT_NONE: return "none";
    case DEVICE_SCHEMA_REFRESH_RESULT_SUCCESS: return "success";
    case DEVICE_SCHEMA_REFRESH_RESULT_UNCHANGED: return "unchanged";
    case DEVICE_SCHEMA_REFRESH_RESULT_NOT_PERSISTED: return "not_persisted";
    case DEVICE_SCHEMA_REFRESH_RESULT_UNSUPPORTED: return "unsupported";
    case DEVICE_SCHEMA_REFRESH_RESULT_BUSY: return "busy";
    case DEVICE_SCHEMA_REFRESH_RESULT_TIMEOUT: return "timeout";
    case DEVICE_SCHEMA_REFRESH_RESULT_DISCONNECTED: return "disconnected";
    case DEVICE_SCHEMA_REFRESH_RESULT_TRANSPORT_ERROR: return "transport_error";
    case DEVICE_SCHEMA_REFRESH_RESULT_PROTOCOL_ERROR: return "protocol_error";
    case DEVICE_SCHEMA_REFRESH_RESULT_INTERNAL_ERROR: return "internal_error";
    default: return "unknown";
    }
}

void device_schema_reset_for_test(void)
{
    schema_worker_reset_for_test();

    if (schema_runtime_lock()) {
        schema_runtime_reset();
        schema_runtime_unlock();
    }
}
