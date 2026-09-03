#include "device_schema_internal.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "device_store.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "schema_disc";

/* ── Completion context ─────────────────────────────────────────────── */

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t operation_id;
    uint32_t refresh_generation;
} completion_context_t;

/* ── Reset record to idle state ─────────────────────────────────────── */

static void reset_record_error(schema_record_t *record)
{
    record->staging_active = false;
    memset(&record->staging, 0, sizeof(record->staging));
    if (!record->has_committed) {
        record->committed.state = DEVICE_SCHEMA_STATE_ERROR;
    }
    record->operation_state = SCHEMA_OP_IDLE;
}

/* ── Forward declaration ────────────────────────────────────────────── */

static void start_discovery(const char *device_id,
                            schema_operation_kind_t kind,
                            uint32_t generation);

/* ── Submit callback from BLE command executor ──────────────────────── */

static void discovery_done(device_schema_submit_result_t result, void *context)
{
    completion_context_t *done_context = context;
    schema_event_t event = {
        .type = SCHEMA_EVENT_COMPLETION,
        .completion = result,
    };
    if (done_context != NULL) {
        strlcpy(event.device_id, done_context->device_id,
                sizeof(event.device_id));
        event.operation_id = done_context->operation_id;
        event.refresh_generation = done_context->refresh_generation;
        free(done_context);
    }
    /* Post completion event to worker queue. */
    schema_worker_post_event(&event);
}

/* ── Start next pending via global serializer ───────────────────────── */

static void start_next_pending(void)
{
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    schema_operation_kind_t kind = SCHEMA_OP_NONE;
    uint32_t gen = 0;

    if (!schema_runtime_lock()) return;

    schema_record_t *records = schema_runtime_records();
    if (!schema_runtime_owner()->active) {
        for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
            if (!records[i].used ||
                records[i].operation_state != SCHEMA_OP_QUEUED) {
                continue;
            }
            strlcpy(device_id, records[i].committed.device_id,
                    sizeof(device_id));
            kind = records[i].operation_kind;
            gen = records[i].refresh_active.generation;
            break;
        }
    }
    schema_runtime_unlock();

    if (device_id[0] != '\0') {
        start_discovery(device_id, kind, gen);
    }
}

/* ── Discovery submission ───────────────────────────────────────────── */

static void start_discovery(const char *device_id,
                            schema_operation_kind_t kind,
                            uint32_t generation)
{
    if (!schema_runtime_lock()) return;
    schema_record_t *record =
        schema_runtime_find_or_create_locked(device_id);
    if (record == NULL) {
        schema_runtime_unlock();
        return;
    }

    if (schema_runtime_owner()->active) {
        record->operation_state = SCHEMA_OP_QUEUED;
        record->committed.state = DEVICE_SCHEMA_STATE_DISCOVERING;
        schema_runtime_unlock();
        return;
    }

    uint32_t op_id = schema_runtime_next_operation_id();
    schema_runtime_owner()->active = true;
    strlcpy(schema_runtime_owner()->device_id, device_id,
            sizeof(schema_runtime_owner()->device_id));
    schema_runtime_owner()->operation_id = op_id;
    schema_runtime_owner()->kind = kind;
    schema_runtime_owner()->refresh_generation = generation;

    record->operation_id = op_id;
    record->operation_kind = kind;
    record->operation_state = SCHEMA_OP_RUNNING;
    record->staging_active = false;
    record->staging_operation_id = op_id;
    record->committed.state = DEVICE_SCHEMA_STATE_DISCOVERING;

    if (kind == SCHEMA_OP_MANUAL) {
        record->refresh_active.generation = generation;
        record->refresh_active.state = DEVICE_SCHEMA_REFRESH_RUNNING;
    }

    schema_runtime_unlock();

    if (schema_runtime_get_submitter() == NULL) {
        if (schema_runtime_lock()) {
            schema_record_t *r = schema_runtime_find_locked(device_id);
            if (r != NULL) reset_record_error(r);
            schema_runtime_owner()->active = false;
            schema_runtime_unlock();
        }
        start_next_pending();
        return;
    }

    completion_context_t *context =
        calloc(1, sizeof(*context));
    if (context == NULL) {
        if (schema_runtime_lock()) {
            schema_record_t *r = schema_runtime_find_locked(device_id);
            if (r != NULL) reset_record_error(r);
            schema_runtime_owner()->active = false;
            schema_runtime_unlock();
        }
        start_next_pending();
        return;
    }
    strlcpy(context->device_id, device_id, sizeof(context->device_id));
    context->operation_id = op_id;
    context->refresh_generation = generation;

    gw_message_t query = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .has_device_id = 1,
    };
    strlcpy(query.type, "device_command", sizeof(query.type));
    strlcpy(query.device_id, device_id, sizeof(query.device_id));
    strlcpy(query.command, DEVICE_SCHEMA_RESERVED_COMMAND,
            sizeof(query.command));

    esp_err_t error = schema_runtime_get_submitter()(
        &query, discovery_done, context);
    if (error != ESP_OK) {
        free(context);
        if (schema_runtime_lock()) {
            schema_record_t *r = schema_runtime_find_locked(device_id);
            if (r != NULL) reset_record_error(r);
            schema_runtime_owner()->active = false;
            schema_runtime_unlock();
        }
        start_next_pending();
    }
}

/* ── Disconnect handling ────────────────────────────────────────────── */

static void handle_disconnect(const char *device_id)
{
    if (!schema_runtime_lock()) return;
    schema_record_t *record = schema_runtime_find_locked(device_id);
    if (record == NULL) {
        schema_runtime_unlock();
        return;
    }

    if (record->operation_state == SCHEMA_OP_RUNNING &&
        schema_runtime_owner()->active &&
        strcmp(schema_runtime_owner()->device_id, device_id) == 0 &&
        schema_runtime_owner()->operation_id == record->operation_id) {
        ESP_LOGW(TAG, "[%s] disconnect during running operation, "
                      "operation_id=%lu",
                 device_id, (unsigned long)record->operation_id);

        record->staging_active = false;
        memset(&record->staging, 0, sizeof(record->staging));

        if (record->operation_kind == SCHEMA_OP_MANUAL) {
            record->refresh_active.state = DEVICE_SCHEMA_REFRESH_IDLE;
            record->refresh_active.generation = 0;
            record->refresh_last_completed.generation =
                record->refresh_active.generation;
            record->refresh_last_completed.result =
                DEVICE_SCHEMA_REFRESH_RESULT_DISCONNECTED;
            record->refresh_last_completed.finished_at_ms =
                esp_timer_get_time() / 1000;
        } else {
            if (!record->has_committed) {
                record->committed.state = DEVICE_SCHEMA_STATE_ERROR;
            }
        }
        record->operation_state = SCHEMA_OP_IDLE;
        schema_runtime_owner()->active = false;
    } else if (record->operation_state == SCHEMA_OP_QUEUED) {
        ESP_LOGW(TAG, "[%s] disconnect while queued, cancelling", device_id);

        if (record->operation_kind == SCHEMA_OP_MANUAL) {
            record->refresh_active.state = DEVICE_SCHEMA_REFRESH_IDLE;
            record->refresh_active.generation = 0;
            record->refresh_last_completed.result =
                DEVICE_SCHEMA_REFRESH_RESULT_DISCONNECTED;
            record->refresh_last_completed.finished_at_ms =
                esp_timer_get_time() / 1000;
        } else {
            if (!record->has_committed) {
                record->committed.state = DEVICE_SCHEMA_STATE_ERROR;
            }
        }
        record->operation_state = SCHEMA_OP_IDLE;
    }

    schema_runtime_unlock();
    start_next_pending();
}

/* ── Completion handling ────────────────────────────────────────────── */

static void handle_completion(const char *device_id,
                              uint32_t event_op_id,
                              uint32_t event_generation,
                              device_schema_submit_result_t completion)
{
    if (!schema_runtime_lock()) return;
    schema_record_t *record = schema_runtime_find_locked(device_id);
    if (record == NULL) {
        schema_runtime_owner()->active = false;
        schema_runtime_unlock();
        start_next_pending();
        return;
    }

    if (!schema_runtime_owner()->active ||
        strcmp(schema_runtime_owner()->device_id, device_id) != 0 ||
        schema_runtime_owner()->operation_id != event_op_id) {
        ESP_LOGW(TAG, "[%s] stale completion ignored (op_id=%lu, owner=%lu)",
                 device_id, (unsigned long)event_op_id,
                 (unsigned long)(schema_runtime_owner()->active
                                     ? schema_runtime_owner()->operation_id
                                     : 0));
        schema_runtime_unlock();
        return;
    }

    if (record->has_committed &&
        record->committed.state == DEVICE_SCHEMA_STATE_READY &&
        completion == DEVICE_SCHEMA_SUBMIT_OK) {
        /* Schema committed before final ACK — normal for schema flow. */
    } else if (completion == DEVICE_SCHEMA_SUBMIT_REJECTED ||
               completion == DEVICE_SCHEMA_SUBMIT_TIMEOUT ||
               completion == DEVICE_SCHEMA_SUBMIT_BUSY ||
               completion != DEVICE_SCHEMA_SUBMIT_OK) {
        if (!record->has_committed) {
            record->committed.state = DEVICE_SCHEMA_STATE_ERROR;
        }
    }

    if (record->operation_kind == SCHEMA_OP_MANUAL) {
        device_schema_refresh_result_t refresh_result;
        switch (completion) {
        case DEVICE_SCHEMA_SUBMIT_OK:
            refresh_result = record->has_committed
                                 ? DEVICE_SCHEMA_REFRESH_RESULT_SUCCESS
                                 : DEVICE_SCHEMA_REFRESH_RESULT_INTERNAL_ERROR;
            break;
        case DEVICE_SCHEMA_SUBMIT_BUSY:
            refresh_result = DEVICE_SCHEMA_REFRESH_RESULT_BUSY;
            break;
        case DEVICE_SCHEMA_SUBMIT_TIMEOUT:
            refresh_result = DEVICE_SCHEMA_REFRESH_RESULT_TIMEOUT;
            break;
        case DEVICE_SCHEMA_SUBMIT_NOT_CONNECTED:
            refresh_result = DEVICE_SCHEMA_REFRESH_RESULT_DISCONNECTED;
            break;
        case DEVICE_SCHEMA_SUBMIT_TRANSPORT_ERROR:
            refresh_result = DEVICE_SCHEMA_REFRESH_RESULT_TRANSPORT_ERROR;
            break;
        case DEVICE_SCHEMA_SUBMIT_REJECTED:
            refresh_result = DEVICE_SCHEMA_REFRESH_RESULT_UNSUPPORTED;
            break;
        default:
            refresh_result = DEVICE_SCHEMA_REFRESH_RESULT_INTERNAL_ERROR;
            break;
        }
        record->refresh_active.state = DEVICE_SCHEMA_REFRESH_IDLE;
        record->refresh_active.generation = 0;
        record->refresh_last_completed.generation = event_generation;
        record->refresh_last_completed.result = refresh_result;
        record->refresh_last_completed.finished_at_ms =
            esp_timer_get_time() / 1000;

        ESP_LOGI(TAG, "[%s] SCHEMA_REFRESH_RESULT gen=%lu result=%s",
                 device_id, (unsigned long)event_generation,
                 device_schema_refresh_result_name(refresh_result));
    }

    record->operation_state = SCHEMA_OP_IDLE;
    schema_runtime_owner()->active = false;
    schema_runtime_unlock();
    start_next_pending();
}

/* ── Internal API for worker ────────────────────────────────────────── */

void schema_discovery_handle_ready(const char *device_id)
{
    if (!schema_runtime_lock()) return;
    schema_record_t *record = schema_runtime_find_locked(device_id);
    if (record == NULL) {
        schema_runtime_unlock();
        start_discovery(device_id, SCHEMA_OP_INITIAL, 0);
        return;
    }
    if (record->has_committed &&
        record->committed.state == DEVICE_SCHEMA_STATE_READY) {
        ESP_LOGI(TAG, "[%s] SCHEMA_READY_CACHE_HIT", device_id);
        schema_runtime_unlock();
        return;
    }
    if (record->operation_state == SCHEMA_OP_QUEUED ||
        record->operation_state == SCHEMA_OP_RUNNING) {
        ESP_LOGI(TAG, "[%s] SCHEMA_READY_DUP operation in progress",
                 device_id);
        schema_runtime_unlock();
        return;
    }
    if (record->has_committed &&
        (record->committed.state == DEVICE_SCHEMA_STATE_ERROR ||
         record->committed.state == DEVICE_SCHEMA_STATE_UNSUPPORTED)) {
        ESP_LOGI(TAG, "[%s] SCHEMA_READY_NO_AUTO_RETRY state=%s",
                 device_id,
                 device_schema_state_name(record->committed.state));
        schema_runtime_unlock();
        return;
    }
    schema_runtime_unlock();
    ESP_LOGI(TAG, "[%s] SCHEMA_READY_CACHE_MISS starting discovery",
             device_id);
    start_discovery(device_id, SCHEMA_OP_INITIAL, 0);
}

void schema_discovery_start(const char *device_id,
                            schema_operation_kind_t kind,
                            uint32_t generation)
{
    start_discovery(device_id, kind, generation);
}

void schema_discovery_handle_disconnect(const char *device_id)
{
    handle_disconnect(device_id);
}

void schema_discovery_handle_completion(const char *device_id,
                                        uint32_t event_op_id,
                                        uint32_t event_generation,
                                        device_schema_submit_result_t completion)
{
    handle_completion(device_id, event_op_id, event_generation, completion);
}
