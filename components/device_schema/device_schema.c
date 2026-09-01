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
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway_events.h"
#include "memory_policy.h"

#define SCHEMA_EVENT_QUEUE_DEPTH 32
#define SCHEMA_WORKER_STACK 6144
#define SCHEMA_WORKER_PRIORITY (tskIDLE_PRIORITY + 3)

static const char *TAG = "device_schema";

/* ── Event types for worker queue ───────────────────────────────────── */

typedef enum {
    SCHEMA_EVENT_READY = 0,
    SCHEMA_EVENT_REFRESH,
    SCHEMA_EVENT_DISCONNECT,
    SCHEMA_EVENT_NOTIFY,
    SCHEMA_EVENT_COMPLETION,
} schema_event_type_t;

typedef struct {
    schema_event_type_t type;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t operation_id;
    uint32_t refresh_generation;
    gw_message_t *message;
    device_schema_submit_result_t completion;
} schema_event_t;

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t operation_id;
    uint32_t refresh_generation;
} completion_context_t;

/* ── Global serializer owner ────────────────────────────────────────── */

typedef struct {
    bool active;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t operation_id;
    int kind;
    uint32_t refresh_generation;
} schema_global_owner_t;

/* ── Module state ───────────────────────────────────────────────────── */

static schema_record_t *s_records;
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static device_schema_submit_fn s_submitter;
static device_schema_commit_listener_t s_commit_listener;
static void *s_commit_listener_context;
static device_schema_commit_listener2_t s_commit_listener2;
static void *s_commit_listener2_context;
static bool s_initialized;
static volatile bool s_shutdown;  /* test-only: signals worker to exit */
static schema_global_owner_t s_owner;
static uint32_t s_next_operation_id;
static uint32_t s_next_global_generation;

/* Queue health metrics */
static uint32_t s_q_enqueued;
static uint32_t s_q_dropped;
static uint32_t s_q_high_watermark;
static uint32_t s_q_message_alloc_fail;
static int64_t s_last_drop_log_us;

/* ── Forward declarations ───────────────────────────────────────────── */

static bool lock_records(void);
static void unlock_records(void);
static void start_discovery(const char *device_id, int kind,
                            uint32_t generation);

/* ── Helpers ────────────────────────────────────────────────────────── */

static uint32_t next_operation_id(void)
{
    uint32_t id;
    do {
        id = ++s_next_operation_id;
    } while (id == 0);
    return id;
}

static uint32_t next_generation(void)
{
    uint32_t gen;
    do {
        gen = ++s_next_global_generation;
    } while (gen == 0);
    return gen;
}

static bool lock_records(void)
{
    return s_mutex != NULL &&
           xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static void unlock_records(void)
{
    xSemaphoreGive(s_mutex);
}

static int find_record_locked(const char *device_id)
{
    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (s_records[i].used &&
            strcmp(s_records[i].committed.device_id, device_id) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_or_create_record_locked(const char *device_id)
{
    int existing = find_record_locked(device_id);
    if (existing >= 0) return existing;

    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (s_records[i].used) continue;
        memset(&s_records[i], 0, sizeof(s_records[i]));
        s_records[i].used = true;
        strlcpy(s_records[i].committed.device_id, device_id,
                sizeof(s_records[i].committed.device_id));
        s_records[i].committed.state = DEVICE_SCHEMA_STATE_UNKNOWN;
        return i;
    }
    return -1;
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

/* ── Completion context helpers ─────────────────────────────────────── */

static completion_context_t *make_completion_context(
    const char *device_id, uint32_t operation_id, uint32_t refresh_generation)
{
    completion_context_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx != NULL) {
        strlcpy(ctx->device_id, device_id, sizeof(ctx->device_id));
        ctx->operation_id = operation_id;
        ctx->refresh_generation = refresh_generation;
    }
    return ctx;
}

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
    if (s_queue == NULL ||
        xQueueSend(s_queue, &event, 0) != pdTRUE) {
        __atomic_fetch_add(&s_q_dropped, 1, __ATOMIC_RELAXED);
        ESP_LOGW(TAG, "Dropping discovery completion for %s", event.device_id);
        if (lock_records()) {
            int index = find_record_locked(event.device_id);
            if (index >= 0) {
                schema_record_t *record = &s_records[index];
                record->staging_active = false;
                memset(&record->staging, 0, sizeof(record->staging));
                if (!record->has_committed) {
                    record->committed.state = DEVICE_SCHEMA_STATE_ERROR;
                }
                record->operation_state = 0 /* IDLE */;
            }
            if (s_owner.active &&
                strcmp(s_owner.device_id, event.device_id) == 0 &&
                s_owner.operation_id == event.operation_id) {
                s_owner.active = false;
            }
            unlock_records();
        }
    }
}

/* ── Start next pending via global serializer ───────────────────────── */

static void start_next_pending(void)
{
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    int kind = 0;
    uint32_t gen = 0;

    if (!lock_records()) return;

    if (!s_owner.active) {
        for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
            if (!s_records[i].used ||
                s_records[i].operation_state != 1 /* QUEUED */) {
                continue;
            }
            strlcpy(device_id, s_records[i].committed.device_id,
                    sizeof(device_id));
            kind = s_records[i].operation_kind;
            gen = s_records[i].refresh_active.generation;
            break;
        }
    }
    unlock_records();

    if (device_id[0] != '\0') {
        start_discovery(device_id, kind, gen);
    }
}

/* ── Discovery submission ───────────────────────────────────────────── */

static void start_discovery(const char *device_id, int kind,
                            uint32_t generation)
{
    if (!lock_records()) return;
    int index = find_or_create_record_locked(device_id);
    if (index < 0) {
        unlock_records();
        return;
    }
    schema_record_t *record = &s_records[index];

    if (s_owner.active) {
        record->operation_state = 1 /* QUEUED */;
        record->committed.state = DEVICE_SCHEMA_STATE_DISCOVERING;
        unlock_records();
        return;
    }

    uint32_t op_id = next_operation_id();
    s_owner.active = true;
    strlcpy(s_owner.device_id, device_id, sizeof(s_owner.device_id));
    s_owner.operation_id = op_id;
    s_owner.kind = kind;
    s_owner.refresh_generation = generation;

    record->operation_id = op_id;
    record->operation_kind = kind;
    record->operation_state = 2 /* RUNNING */;
    record->staging_active = false;
    record->staging_operation_id = op_id;
    record->committed.state = DEVICE_SCHEMA_STATE_DISCOVERING;

    if (kind == 2 /* MANUAL */) {
        record->refresh_active.generation = generation;
        record->refresh_active.state = DEVICE_SCHEMA_REFRESH_RUNNING;
    }

    unlock_records();

    if (s_submitter == NULL) {
        if (lock_records()) {
            int idx = find_record_locked(device_id);
            if (idx >= 0) {
                schema_record_t *r = &s_records[idx];
                r->staging_active = false;
                memset(&r->staging, 0, sizeof(r->staging));
                if (!r->has_committed) {
                    r->committed.state = DEVICE_SCHEMA_STATE_ERROR;
                }
                r->operation_state = 0 /* IDLE */;
            }
            s_owner.active = false;
            unlock_records();
        }
        start_next_pending();
        return;
    }

    completion_context_t *context =
        make_completion_context(device_id, op_id, generation);
    if (context == NULL) {
        if (lock_records()) {
            int idx = find_record_locked(device_id);
            if (idx >= 0) {
                schema_record_t *r = &s_records[idx];
                r->staging_active = false;
                memset(&r->staging, 0, sizeof(r->staging));
                if (!r->has_committed) {
                    r->committed.state = DEVICE_SCHEMA_STATE_ERROR;
                }
                r->operation_state = 0 /* IDLE */;
            }
            s_owner.active = false;
            unlock_records();
        }
        start_next_pending();
        return;
    }

    gw_message_t query = {
        .protocol_version = GW_PROTOCOL_VERSION,
        .has_device_id = 1,
    };
    strlcpy(query.type, "device_command", sizeof(query.type));
    strlcpy(query.device_id, device_id, sizeof(query.device_id));
    strlcpy(query.command, DEVICE_SCHEMA_RESERVED_COMMAND,
            sizeof(query.command));

    esp_err_t error = s_submitter(&query, discovery_done, context);
    if (error != ESP_OK) {
        free(context);
        if (lock_records()) {
            int idx = find_record_locked(device_id);
            if (idx >= 0) {
                schema_record_t *r = &s_records[idx];
                r->staging_active = false;
                memset(&r->staging, 0, sizeof(r->staging));
                if (!r->has_committed) {
                    r->committed.state = DEVICE_SCHEMA_STATE_ERROR;
                }
                r->operation_state = 0 /* IDLE */;
            }
            s_owner.active = false;
            unlock_records();
        }
        start_next_pending();
    }
}

/* ── Wire message helpers ───────────────────────────────────────────── */

static bool message_device_matches(const char *device_id,
                                   const gw_message_t *message)
{
    return message->protocol_version == GW_PROTOCOL_VERSION &&
           message->has_device_id &&
           strcmp(device_id, message->device_id) == 0;
}

/* ── BEGIN / ITEM / FEATURE_ITEM / END handlers ─────────────────────── */

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
    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        schema_record_t *record = &s_records[index];
        if (record->operation_state != 2 /* RUNNING */) {
            unlock_records();
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
    unlock_records();
}

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

    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        schema_record_t *record = &s_records[index];
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
            record->operation_state = 0 /* IDLE */;
        }
    }
    unlock_records();
}

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
    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        schema_record_t *record = &s_records[index];
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
            record->operation_state = 0 /* IDLE */;
        }
    }
    unlock_records();
}

static void handle_end(const char *device_id, const gw_message_t *message)
{
    if (!message_device_matches(device_id, message) ||
        !message->has_snapshot_id || !message->has_total) {
        return;
    }

    device_schema_snapshot_t committed;
    int persist_index = -1;
    bool changed = false;
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        schema_record_t *record = &s_records[index];
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
            persist_index = index;

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
            record->operation_state = 0 /* IDLE */;
        }
    }
    unlock_records();

    if (persist_index >= 0) {
        if (!changed) {
            if (lock_records()) {
                int idx = find_record_locked(device_id);
                if (idx >= 0 && s_records[idx].persist_dirty) {
                    esp_err_t err = schema_persist_record(idx, &committed);
                    if (err == ESP_OK) {
                        s_records[idx].persist_dirty = false;
                    }
                }
                unlock_records();
            }
        } else {
            esp_err_t error = schema_persist_record(persist_index, &committed);
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "[%s] NVS persist failed: %s (persist_dirty=true)",
                         device_id, esp_err_to_name(error));
                if (lock_records()) {
                    int idx = find_record_locked(device_id);
                    if (idx >= 0) {
                        s_records[idx].persist_dirty = true;
                    }
                    unlock_records();
                }
            } else {
                ESP_LOGI(TAG, "[%s] persisted schema to NVS", device_id);
            }
        }
    }

    /* Commit listener fires outside the schema mutex, after the persist
     * decision, for every successful commit (changed or not). It must only
     * enqueue work — the exposure consumer relies on this contract. */
    if (persist_index >= 0 && s_commit_listener != NULL) {
        s_commit_listener(device_id, committed.revision,
                          s_commit_listener_context);
    }
    if (persist_index >= 0 && s_commit_listener2 != NULL) {
        s_commit_listener2(device_id, committed.revision,
                           s_commit_listener2_context);
    }

    /* Publish schema event for realtime consumers (WebSocket, etc.) */
    if (persist_index >= 0) {
        gateway_event_t ev = {0};
        ev.type = GW_EVENT_DEVICE_SCHEMA;
        strlcpy(ev.device_id, device_id, sizeof(ev.device_id));
        ev.schema_revision = committed.revision;
        gateway_events_publish(&ev);
    }
}

/* ── Disconnect handling ────────────────────────────────────────────── */

static void handle_disconnect(const char *device_id)
{
    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index < 0) {
        unlock_records();
        return;
    }
    schema_record_t *record = &s_records[index];

    if (record->operation_state == 2 /* RUNNING */ &&
        s_owner.active &&
        strcmp(s_owner.device_id, device_id) == 0 &&
        s_owner.operation_id == record->operation_id) {
        ESP_LOGW(TAG, "[%s] disconnect during running operation, "
                      "operation_id=%lu",
                 device_id, (unsigned long)record->operation_id);

        record->staging_active = false;
        memset(&record->staging, 0, sizeof(record->staging));

        if (record->operation_kind == 2 /* MANUAL */) {
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
        record->operation_state = 0 /* IDLE */;
        s_owner.active = false;
    } else if (record->operation_state == 1 /* QUEUED */) {
        ESP_LOGW(TAG, "[%s] disconnect while queued, cancelling", device_id);

        if (record->operation_kind == 2 /* MANUAL */) {
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
        record->operation_state = 0 /* IDLE */;
    }

    unlock_records();
    start_next_pending();
}

/* ── Completion handling ────────────────────────────────────────────── */

static void handle_completion(const char *device_id,
                              uint32_t event_op_id,
                              uint32_t event_generation,
                              device_schema_submit_result_t completion)
{
    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index < 0) {
        s_owner.active = false;
        unlock_records();
        start_next_pending();
        return;
    }
    schema_record_t *record = &s_records[index];

    if (!s_owner.active ||
        strcmp(s_owner.device_id, device_id) != 0 ||
        s_owner.operation_id != event_op_id) {
        ESP_LOGW(TAG, "[%s] stale completion ignored (op_id=%lu, owner=%lu)",
                 device_id, (unsigned long)event_op_id,
                 (unsigned long)(s_owner.active ? s_owner.operation_id : 0));
        unlock_records();
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

    if (record->operation_kind == 2 /* MANUAL */) {
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

    record->operation_state = 0 /* IDLE */;
    s_owner.active = false;
    unlock_records();
    start_next_pending();
}

/* ── Worker task ────────────────────────────────────────────────────── */

static void schema_worker(void *arg)
{
    (void)arg;
    schema_event_t event;
    for (;;) {
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (s_shutdown) break;
            continue;
        }
        if (s_shutdown) break;
        switch (event.type) {
        case SCHEMA_EVENT_READY: {
            if (!lock_records()) break;
            int index = find_record_locked(event.device_id);
            if (index < 0) {
                unlock_records();
                start_discovery(event.device_id, 1 /* INITIAL */, 0);
                break;
            }
            schema_record_t *record = &s_records[index];
            if (record->has_committed &&
                record->committed.state == DEVICE_SCHEMA_STATE_READY) {
                ESP_LOGI(TAG, "[%s] SCHEMA_READY_CACHE_HIT", event.device_id);
                unlock_records();
                break;
            }
            if (record->operation_state == 1 /* QUEUED */ ||
                record->operation_state == 2 /* RUNNING */) {
                ESP_LOGI(TAG, "[%s] SCHEMA_READY_DUP operation in progress",
                         event.device_id);
                unlock_records();
                break;
            }
            if (record->has_committed &&
                (record->committed.state == DEVICE_SCHEMA_STATE_ERROR ||
                 record->committed.state == DEVICE_SCHEMA_STATE_UNSUPPORTED)) {
                ESP_LOGI(TAG, "[%s] SCHEMA_READY_NO_AUTO_RETRY state=%s",
                         event.device_id,
                         device_schema_state_name(
                             record->committed.state));
                unlock_records();
                break;
            }
            unlock_records();
            ESP_LOGI(TAG, "[%s] SCHEMA_READY_CACHE_MISS starting discovery",
                     event.device_id);
            start_discovery(event.device_id, 1 /* INITIAL */, 0);
            break;
        }
        case SCHEMA_EVENT_REFRESH:
            start_discovery(event.device_id, 2 /* MANUAL */,
                            event.refresh_generation);
            break;
        case SCHEMA_EVENT_DISCONNECT:
            handle_disconnect(event.device_id);
            break;
        case SCHEMA_EVENT_NOTIFY:
            if (event.message != NULL) {
                if (strcmp(event.message->type, "capabilities_begin") == 0) {
                    handle_begin(event.device_id, event.message);
                } else if (strcmp(event.message->type,
                                  "capability_item") == 0) {
                    handle_tool_item(event.device_id, event.message);
                } else if (strcmp(event.message->type,
                                  "feature_item") == 0) {
                    handle_feature_item(event.device_id, event.message);
                } else {
                    handle_end(event.device_id, event.message);
                }
                gw_mem_free(event.message);
            }
            break;
        case SCHEMA_EVENT_COMPLETION:
            handle_completion(event.device_id, event.operation_id,
                              event.refresh_generation, event.completion);
            break;
        }
    }
    /* Worker must never return — FreeRTOS aborts tasks that do.
       After s_shutdown breaks the loop, park here until the idle task
       cleans up or reset_for_test orphans us. */
    for (;;) { vTaskDelay(portMAX_DELAY); }
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t device_schema_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    if (s_records == NULL) {
        s_records = gw_mem_calloc(DEVICE_STORE_MAX_DEVICES,
                                  sizeof(*s_records),
                                  GW_MEM_EXTERNAL_PREFERRED);
        if (s_records == NULL) return ESP_ERR_NO_MEM;
    }
    memset(s_records, 0,
           DEVICE_STORE_MAX_DEVICES * sizeof(*s_records));
    memset(&s_owner, 0, sizeof(s_owner));
    s_commit_listener2 = NULL;
    s_commit_listener2_context = NULL;
    s_next_operation_id = 0;
    s_next_global_generation = 0;

    schema_load_persisted(s_records);
    schema_cleanup_legacy_caps();

    /* Create queue and worker only on the first init.  Test reset preserves
       them so tests don't accumulate orphaned FreeRTOS tasks. */
    if (s_queue == NULL) {
        s_queue = xQueueCreate(SCHEMA_EVENT_QUEUE_DEPTH, sizeof(schema_event_t));
        if (s_queue == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_worker == NULL) {
        if (xTaskCreate(schema_worker, "device_schema", SCHEMA_WORKER_STACK,
                        NULL, SCHEMA_WORKER_PRIORITY, &s_worker) != pdPASS) {
            vQueueDelete(s_queue);
            s_queue = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    s_initialized = true;
    return ESP_OK;
}

void device_schema_set_submitter(device_schema_submit_fn submitter)
{
    s_submitter = submitter;
}

esp_err_t device_schema_register_commit_listener(
    device_schema_commit_listener_t listener, void *context)
{
    if (!lock_records()) return ESP_ERR_TIMEOUT;
    s_commit_listener = listener;
    s_commit_listener_context = context;
    unlock_records();
    return ESP_OK;
}

esp_err_t device_schema_register_commit_listener2(
    device_schema_commit_listener2_t listener, void *context)
{
    if (!lock_records()) return ESP_ERR_TIMEOUT;
    s_commit_listener2 = listener;
    s_commit_listener2_context = context;
    unlock_records();
    return ESP_OK;
}

static esp_err_t enqueue_schema_event(schema_event_type_t type,
                                       const char *device_id,
                                       uint32_t operation_id,
                                       uint32_t refresh_generation)
{
    if (!s_initialized || s_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || device_id[0] == '\0' ||
        strnlen(device_id, GW_MSG_DEVICE_ID_LEN) >= GW_MSG_DEVICE_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    schema_event_t event = {
        .type = type,
        .operation_id = operation_id,
        .refresh_generation = refresh_generation,
    };
    strlcpy(event.device_id, device_id, sizeof(event.device_id));
    if (xQueueSend(s_queue, &event, 0) != pdTRUE) {
        __atomic_fetch_add(&s_q_dropped, 1, __ATOMIC_RELAXED);
        return ESP_ERR_NO_MEM;
    }
    __atomic_fetch_add(&s_q_enqueued, 1, __ATOMIC_RELAXED);
    return ESP_OK;
}

esp_err_t device_schema_on_ready(const char *device_id)
{
    return enqueue_schema_event(SCHEMA_EVENT_READY, device_id, 0, 0);
}

void device_schema_on_disconnect(const char *device_id)
{
    enqueue_schema_event(SCHEMA_EVENT_DISCONNECT, device_id, 0, 0);
}

bool device_schema_on_notify(const char *device_id,
                              const gw_message_t *message)
{
    if (message == NULL ||
        (strcmp(message->type, "capabilities_begin") != 0 &&
         strcmp(message->type, "capability_item") != 0 &&
         strcmp(message->type, "feature_item") != 0 &&
         strcmp(message->type, "capabilities_end") != 0)) {
        return false;
    }
    if (!s_initialized || device_id == NULL) return true;

    uint32_t op_id = 0;
    if (lock_records()) {
        int index = find_record_locked(device_id);
        if (index >= 0) {
            op_id = s_records[index].operation_id;
        }
        unlock_records();
    }

    schema_event_t event = {
        .type = SCHEMA_EVENT_NOTIFY,
        .operation_id = op_id,
    };
    strlcpy(event.device_id, device_id, sizeof(event.device_id));

    event.message = gw_mem_alloc(sizeof(*event.message),
                                GW_MEM_EXTERNAL_PREFERRED);
    if (event.message == NULL) {
        __atomic_fetch_add(&s_q_message_alloc_fail, 1, __ATOMIC_RELAXED);
        ESP_LOGE(TAG, "[%s] schema message alloc failed", device_id);
        return true;
    }
    *event.message = *message;

    if (xQueueSend(s_queue, &event, 0) != pdTRUE) {
        gw_mem_free(event.message);
        __atomic_fetch_add(&s_q_dropped, 1, __ATOMIC_RELAXED);
        int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_drop_log_us > 5000000LL) {
            s_last_drop_log_us = now_us;
            ESP_LOGW(TAG, "[%s] schema queue full (dropped total: %u)",
                     device_id,
                     (unsigned)__atomic_load_n(&s_q_dropped,
                                               __ATOMIC_RELAXED));
        }
        return true;
    }

    __atomic_fetch_add(&s_q_enqueued, 1, __ATOMIC_RELAXED);
    UBaseType_t depth = uxQueueMessagesWaiting(s_queue);
    uint32_t prev = __atomic_load_n(&s_q_high_watermark, __ATOMIC_RELAXED);
    while ((uint32_t)depth > prev &&
           !__atomic_compare_exchange_n(&s_q_high_watermark, &prev,
                                        (uint32_t)depth, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    return true;
}

void device_schema_get_queue_stats(device_schema_queue_stats_t *out)
{
    if (out == NULL) return;
    out->enqueued =
        __atomic_load_n(&s_q_enqueued, __ATOMIC_RELAXED);
    out->dropped =
        __atomic_load_n(&s_q_dropped, __ATOMIC_RELAXED);
    out->high_watermark =
        __atomic_load_n(&s_q_high_watermark, __ATOMIC_RELAXED);
    out->message_alloc_fail =
        __atomic_load_n(&s_q_message_alloc_fail, __ATOMIC_RELAXED);
}

esp_err_t device_schema_refresh(const char *device_id,
                                 uint32_t *out_generation)
{
    if (device_id == NULL || device_id[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!s_initialized || s_records == NULL) return ESP_ERR_INVALID_STATE;

    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!lock_records()) return ESP_ERR_TIMEOUT;
    int index = find_or_create_record_locked(device_id);
    if (index < 0) {
        unlock_records();
        return ESP_ERR_NO_MEM;
    }
    schema_record_t *record = &s_records[index];

    if (record->operation_state == 1 /* QUEUED */ ||
        record->operation_state == 2 /* RUNNING */) {
        unlock_records();
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t gen = next_generation();
    uint32_t op_id = next_operation_id();

    record->operation_kind = 2 /* MANUAL */;
    record->operation_state = 1 /* QUEUED */;
    record->operation_id = op_id;
    record->refresh_active.generation = gen;
    record->refresh_active.state = DEVICE_SCHEMA_REFRESH_QUEUED;

    if (out_generation != NULL) *out_generation = gen;

    ESP_LOGI(TAG, "[%s] SCHEMA_REFRESH_RESERVE gen=%lu op_id=%lu",
             device_id, (unsigned long)gen, (unsigned long)op_id);

    unlock_records();

    esp_err_t err = enqueue_schema_event(SCHEMA_EVENT_REFRESH, device_id,
                                          op_id, gen);
    if (err != ESP_OK) {
        if (lock_records()) {
            int idx = find_record_locked(device_id);
            if (idx >= 0) {
                s_records[idx].operation_state = 0 /* IDLE */;
                s_records[idx].refresh_active.state = DEVICE_SCHEMA_REFRESH_IDLE;
                s_records[idx].refresh_active.generation = 0;
            }
            unlock_records();
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
    if (!lock_records()) return ESP_ERR_TIMEOUT;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        *out_snapshot = s_records[index].committed;
        out_snapshot->has_committed = s_records[index].has_committed;
    }
    unlock_records();
    return ESP_OK;
}

esp_err_t device_schema_get_refresh_status(
    const char *device_id,
    device_schema_refresh_active_t *out_active,
    device_schema_refresh_completed_t *out_completed)
{
    if (device_id == NULL) return ESP_ERR_INVALID_ARG;
    if (!lock_records()) return ESP_ERR_TIMEOUT;
    int index = find_record_locked(device_id);
    if (index < 0) {
        unlock_records();
        return ESP_ERR_NOT_FOUND;
    }
    if (out_active != NULL) {
        *out_active = s_records[index].refresh_active;
    }
    if (out_completed != NULL) {
        *out_completed = s_records[index].refresh_last_completed;
    }
    unlock_records();
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
    if (!lock_records()) return DEVICE_SCHEMA_VALID_UNKNOWN;
    int index = find_record_locked(message->device_id);
    if (index < 0 || !s_records[index].has_committed ||
        (s_records[index].committed.state != DEVICE_SCHEMA_STATE_READY &&
         s_records[index].committed.state != DEVICE_SCHEMA_STATE_DISCOVERING)) {
        unlock_records();
        return DEVICE_SCHEMA_VALID_UNKNOWN;
    }

    const device_schema_tool_t *tool = NULL;
    for (size_t i = 0; i < s_records[index].committed.tool_count; i++) {
        if (strcmp(s_records[index].committed.tools[i].command,
                   message->command) == 0) {
            tool = &s_records[index].committed.tools[i];
            break;
        }
    }
    if (tool == NULL) {
        unlock_records();
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
    unlock_records();
    return valid_argument ? DEVICE_SCHEMA_VALID : DEVICE_SCHEMA_VALID_ARGUMENT;
}

esp_err_t device_schema_forget(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return ESP_ERR_INVALID_ARG;

    if (!lock_records()) return ESP_ERR_TIMEOUT;
    int index = find_record_locked(device_id);
    if (index < 0) {
        unlock_records();
        return ESP_OK;
    }

    schema_record_t *record = &s_records[index];
    if (record->operation_state == 1 /* QUEUED */ ||
        record->operation_state == 2 /* RUNNING */) {
        if (s_owner.active &&
            strcmp(s_owner.device_id, device_id) == 0 &&
            s_owner.operation_id == record->operation_id) {
            s_owner.active = false;
        }
        record->operation_state = 0 /* IDLE */;
    }
    record->staging_active = false;
    memset(&record->staging, 0, sizeof(record->staging));
    unlock_records();

    esp_err_t error = schema_erase_nvs(index);

    if (error != ESP_OK) {
        ESP_LOGW(TAG, "[%s] NVS erase failed: %s, RAM cache preserved",
                 device_id, esp_err_to_name(error));
        return error;
    }

    if (lock_records()) {
        int idx = find_record_locked(device_id);
        if (idx >= 0) {
            memset(&s_records[idx], 0, sizeof(s_records[idx]));
        }
        unlock_records();
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
    /* Drain any pending events from the queue without touching the worker.
       The worker stays alive and idle between tests — no orphaned tasks,
       no "should not return" abort.  device_schema_init() skips queue/task
       creation when they already exist. */
    if (s_queue != NULL) {
        schema_event_t event;
        while (xQueueReceive(s_queue, &event, 0) == pdTRUE) {
            if (event.type == SCHEMA_EVENT_NOTIFY && event.message != NULL) {
                gw_mem_free(event.message);
            }
        }
    }

    if (lock_records()) {
        if (s_records != NULL) {
            memset(s_records, 0,
                   DEVICE_STORE_MAX_DEVICES * sizeof(*s_records));
        }
        memset(&s_owner, 0, sizeof(s_owner));
        s_next_operation_id = 0;
        s_next_global_generation = 0;
        s_initialized = false;
        unlock_records();
    }
}
