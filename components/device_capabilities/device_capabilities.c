#include "device_capabilities.h"

#include <ctype.h>
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
#include "nvs.h"

#define CAP_EVENT_QUEUE_DEPTH 32
#define CAP_WORKER_STACK 6144
#define CAP_WORKER_PRIORITY (tskIDLE_PRIORITY + 3)
#define CAP_STORE_SCHEMA_VERSION 1
#define CAP_NVS_NAMESPACE "dev_caps"

static const char *TAG = "device_caps";

/* ── Internal enums ─────────────────────────────────────────────────── */

typedef enum {
    DEVICE_CAP_OP_NONE = 0,
    DEVICE_CAP_OP_INITIAL,
    DEVICE_CAP_OP_MANUAL,
} device_cap_operation_kind_t;

typedef enum {
    DEVICE_CAP_OP_IDLE = 0,
    DEVICE_CAP_OP_QUEUED,
    DEVICE_CAP_OP_RUNNING,
} device_cap_operation_state_t;

/* ── Per-device record ──────────────────────────────────────────────── */

typedef struct {
    bool used;
    bool has_committed;
    bool persist_dirty;

    device_capability_snapshot_t committed;

    bool staging_active;
    uint32_t staging_operation_id;
    uint16_t staging_expected;
    device_capability_snapshot_t staging;

    device_cap_operation_kind_t operation_kind;
    device_cap_operation_state_t operation_state;
    uint32_t operation_id;

    device_cap_refresh_active_t refresh_active;
    device_cap_refresh_completed_t refresh_last_completed;
} capability_record_t;

/* ── NVS persisted blob ─────────────────────────────────────────────── */

typedef struct {
    uint8_t schema_version;
    uint8_t count;
    uint16_t reserved;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t revision;
    device_capability_t items[DEVICE_CAP_MAX_PER_DEVICE];
} persisted_snapshot_t;

/* ── Event types for worker queue ───────────────────────────────────── */

typedef enum {
    CAP_EVENT_READY = 0,
    CAP_EVENT_REFRESH,
    CAP_EVENT_DISCONNECT,
    CAP_EVENT_NOTIFY,
    CAP_EVENT_COMPLETION,
} capability_event_type_t;

typedef struct {
    capability_event_type_t type;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t operation_id;
    uint32_t refresh_generation;
    gw_message_t message;
    device_cap_submit_result_t completion;
} capability_event_t;

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
    device_cap_operation_kind_t kind;
    uint32_t refresh_generation;
} capability_global_owner_t;

/* ── Module state ───────────────────────────────────────────────────── */

static capability_record_t s_records[DEVICE_STORE_MAX_DEVICES];
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static device_cap_submit_fn s_submitter;
static device_capability_commit_listener_t s_commit_listener;
static void *s_commit_listener_context;
static bool s_initialized;
static capability_global_owner_t s_owner;
static uint32_t s_next_operation_id;
static uint32_t s_next_global_generation;

/* ── Forward declarations ───────────────────────────────────────────── */

static bool lock_records(void);
static void unlock_records(void);
static void start_discovery(const char *device_id,
                            device_cap_operation_kind_t kind,
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
        s_records[i].committed.state = DEVICE_CAP_STATE_UNKNOWN;
        return i;
    }
    return -1;
}

static bool valid_command_name(const char *command)
{
    size_t length = strnlen(command, GW_MSG_COMMAND_LEN);
    if (length == 0 || length >= GW_MSG_COMMAND_LEN) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)command[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

static bool valid_capability(const device_capability_t *capability)
{
    if (!valid_command_name(capability->command) ||
        capability->value_type > DEVICE_CAP_VALUE_INT) {
        return false;
    }
    if (capability->value_type == DEVICE_CAP_VALUE_INT) {
        if (capability->min_value > capability->max_value ||
            capability->step == 0) {
            return false;
        }
    }
    return true;
}

static bool capability_item_equal(const device_capability_t *a,
                                  const device_capability_t *b)
{
    return a->value_type == b->value_type &&
           a->flags == b->flags &&
           a->min_value == b->min_value &&
           a->max_value == b->max_value &&
           a->step == b->step &&
           strcmp(a->command, b->command) == 0 &&
           strcmp(a->label, b->label) == 0 &&
           strcmp(a->unit, b->unit) == 0;
}

static bool snapshot_content_equal(const device_capability_snapshot_t *a,
                                   const device_capability_snapshot_t *b)
{
    if (a->revision != b->revision || a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++) {
        if (!capability_item_equal(&a->items[i], &b->items[i])) return false;
    }
    return true;
}

/* ── NVS persistence ────────────────────────────────────────────────── */

static void capability_nvs_key(int index, char key[8])
{
    unsigned bounded = (unsigned)index % DEVICE_STORE_MAX_DEVICES;
    snprintf(key, 8, "cap%02u", bounded);
}

static esp_err_t persist_record(int index,
                                const device_capability_snapshot_t *snapshot)
{
    persisted_snapshot_t persisted = {
        .schema_version = CAP_STORE_SCHEMA_VERSION,
        .count = (uint8_t)snapshot->count,
        .revision = snapshot->revision,
    };
    strlcpy(persisted.device_id, snapshot->device_id,
            sizeof(persisted.device_id));
    if (snapshot->count > 0) {
        memcpy(persisted.items, snapshot->items,
               snapshot->count * sizeof(snapshot->items[0]));
    }

    nvs_handle_t handle;
    esp_err_t error = nvs_open(CAP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    char key[8];
    capability_nvs_key(index, key);
    size_t blob_size = offsetof(persisted_snapshot_t, items) +
                       snapshot->count * sizeof(persisted.items[0]);
    error = nvs_set_blob(handle, key, &persisted, blob_size);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

static void load_persisted(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(CAP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not open capability NVS: %s",
                 esp_err_to_name(error));
        return;
    }

    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        char key[8];
        capability_nvs_key(i, key);
        persisted_snapshot_t persisted;
        memset(&persisted, 0, sizeof(persisted));
        size_t length = 0;
        error = nvs_get_blob(handle, key, NULL, &length);
        if (error == ESP_ERR_NVS_NOT_FOUND) continue;
        if (error != ESP_OK || length < offsetof(persisted_snapshot_t, items) ||
            length > sizeof(persisted)) {
            ESP_LOGW(TAG, "Ignoring invalid capability record %s", key);
            continue;
        }
        error = nvs_get_blob(handle, key, &persisted, &length);
        size_t expected_length = offsetof(persisted_snapshot_t, items) +
                                 persisted.count * sizeof(persisted.items[0]);
        if (error != ESP_OK || length != expected_length ||
            persisted.schema_version != CAP_STORE_SCHEMA_VERSION ||
            persisted.count > DEVICE_CAP_MAX_PER_DEVICE ||
            persisted.device_id[0] == '\0') {
            ESP_LOGW(TAG, "Ignoring invalid capability record %s", key);
            continue;
        }

        device_entry_t device;
        if (device_store_get(persisted.device_id, &device) != DEVICE_STORE_OK) {
            continue;
        }

        bool valid = true;
        for (size_t item = 0; item < persisted.count; item++) {
            if (!valid_capability(&persisted.items[item])) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;

        capability_record_t *record = &s_records[i];
        memset(record, 0, sizeof(*record));
        record->used = true;
        record->has_committed = true;
        record->persist_dirty = false;
        strlcpy(record->committed.device_id, persisted.device_id,
                sizeof(record->committed.device_id));
        record->committed.state = DEVICE_CAP_STATE_READY;
        record->committed.revision = persisted.revision;
        record->committed.count = persisted.count;
        memcpy(record->committed.items, persisted.items,
               persisted.count * sizeof(persisted.items[0]));

        ESP_LOGI(TAG, "[%s] loaded cached capabilities (revision=%lu, %u items)",
                 persisted.device_id, (unsigned long)persisted.revision,
                 (unsigned)persisted.count);
    }
    nvs_close(handle);
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

static void discovery_done(device_cap_submit_result_t result, void *context)
{
    completion_context_t *done_context = context;
    capability_event_t event = {
        .type = CAP_EVENT_COMPLETION,
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
        ESP_LOGW(TAG, "Dropping discovery completion for %s", event.device_id);
        if (lock_records()) {
            int index = find_record_locked(event.device_id);
            if (index >= 0) {
                capability_record_t *record = &s_records[index];
                record->staging_active = false;
                memset(&record->staging, 0, sizeof(record->staging));
                if (!record->has_committed) {
                    record->committed.state = DEVICE_CAP_STATE_ERROR;
                }
                record->operation_state = DEVICE_CAP_OP_IDLE;
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
    device_cap_operation_kind_t kind = DEVICE_CAP_OP_NONE;
    uint32_t gen = 0;

    if (!lock_records()) return;

    if (!s_owner.active) {
        for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
            if (!s_records[i].used ||
                s_records[i].operation_state != DEVICE_CAP_OP_QUEUED) {
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

static void start_discovery(const char *device_id,
                            device_cap_operation_kind_t kind,
                            uint32_t generation)
{
    if (!lock_records()) return;
    int index = find_or_create_record_locked(device_id);
    if (index < 0) {
        unlock_records();
        return;
    }
    capability_record_t *record = &s_records[index];

    if (s_owner.active) {
        record->operation_state = DEVICE_CAP_OP_QUEUED;
        record->committed.state = DEVICE_CAP_STATE_DISCOVERING;
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
    record->operation_state = DEVICE_CAP_OP_RUNNING;
    record->staging_active = false;
    record->staging_operation_id = op_id;
    record->committed.state = DEVICE_CAP_STATE_DISCOVERING;

    if (kind == DEVICE_CAP_OP_MANUAL) {
        record->refresh_active.generation = generation;
        record->refresh_active.state = DEVICE_CAP_REFRESH_RUNNING;
    }

    unlock_records();

    if (s_submitter == NULL) {
        if (lock_records()) {
            int idx = find_record_locked(device_id);
            if (idx >= 0) {
                capability_record_t *r = &s_records[idx];
                r->staging_active = false;
                memset(&r->staging, 0, sizeof(r->staging));
                if (!r->has_committed) {
                    r->committed.state = DEVICE_CAP_STATE_ERROR;
                }
                r->operation_state = DEVICE_CAP_OP_IDLE;
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
                capability_record_t *r = &s_records[idx];
                r->staging_active = false;
                memset(&r->staging, 0, sizeof(r->staging));
                if (!r->has_committed) {
                    r->committed.state = DEVICE_CAP_STATE_ERROR;
                }
                r->operation_state = DEVICE_CAP_OP_IDLE;
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
    strlcpy(query.command, DEVICE_CAP_RESERVED_COMMAND, sizeof(query.command));

    esp_err_t error = s_submitter(&query, discovery_done, context);
    if (error != ESP_OK) {
        free(context);
        if (lock_records()) {
            int idx = find_record_locked(device_id);
            if (idx >= 0) {
                capability_record_t *r = &s_records[idx];
                r->staging_active = false;
                memset(&r->staging, 0, sizeof(r->staging));
                if (!r->has_committed) {
                    r->committed.state = DEVICE_CAP_STATE_ERROR;
                }
                r->operation_state = DEVICE_CAP_OP_IDLE;
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
    return message->protocol_version >= 3 && message->has_device_id &&
           strcmp(device_id, message->device_id) == 0;
}

/* ── BEGIN / ITEM / END handlers ────────────────────────────────────── */

static void handle_begin(const char *device_id, const gw_message_t *message)
{
    if (!message_device_matches(device_id, message) ||
        !message->has_snapshot_id || !message->has_total ||
        !message->has_capability_revision ||
        message->total > DEVICE_CAP_MAX_PER_DEVICE) {
        return;
    }
    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        capability_record_t *record = &s_records[index];
        if (record->operation_state != DEVICE_CAP_OP_RUNNING) {
            unlock_records();
            return;
        }
        memset(&record->staging, 0, sizeof(record->staging));
        strlcpy(record->staging.device_id, device_id,
                sizeof(record->staging.device_id));
        record->staging.state = DEVICE_CAP_STATE_DISCOVERING;
        record->staging.snapshot_id = message->snapshot_id;
        record->staging.revision = message->capability_revision;
        record->staging_expected = message->total;
        record->staging_active = true;
        ESP_LOGI(TAG, "[%s] CAP_BEGIN snapshot=%lu total=%u revision=%lu",
                 device_id, (unsigned long)message->snapshot_id,
                 (unsigned)message->total,
                 (unsigned long)message->capability_revision);
    }
    unlock_records();
}

static void handle_item(const char *device_id, const gw_message_t *message)
{
    if (!message_device_matches(device_id, message) ||
        !message->has_snapshot_id || !message->has_sequence ||
        !message->has_value_type) {
        return;
    }

    device_capability_t item = {
        .value_type = (device_cap_value_type_t)message->value_type,
        .flags = message->has_capability_flags
                     ? message->capability_flags
                     : 0,
        .min_value = message->min_value,
        .max_value = message->max_value,
        .step = message->step,
    };
    strlcpy(item.command, message->command, sizeof(item.command));
    strlcpy(item.label,
            message->capability_label[0] != '\0'
                ? message->capability_label
                : message->command,
            sizeof(item.label));
    strlcpy(item.unit, message->capability_unit, sizeof(item.unit));

    if (item.value_type == DEVICE_CAP_VALUE_INT &&
        (!message->has_min_value || !message->has_max_value ||
         !message->has_step)) {
        return;
    }
    if (!valid_capability(&item)) return;

    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        capability_record_t *record = &s_records[index];
        size_t next = record->staging.count;
        bool valid_sequence =
            record->staging_active &&
            record->staging.snapshot_id == message->snapshot_id &&
            message->sequence == next && next < record->staging_expected;
        if (valid_sequence) {
            for (size_t i = 0; i < next; i++) {
                if (strcmp(record->staging.items[i].command,
                           item.command) == 0) {
                    valid_sequence = false;
                    break;
                }
            }
        }
        if (valid_sequence) {
            record->staging.items[next] = item;
            record->staging.count++;
        } else {
            ESP_LOGW(TAG, "[%s] CAP_ITEM invalid seq=%u", device_id,
                     (unsigned)message->sequence);
            record->staging_active = false;
            memset(&record->staging, 0, sizeof(record->staging));
            if (!record->has_committed) {
                record->committed.state = DEVICE_CAP_STATE_ERROR;
            }
            record->operation_state = DEVICE_CAP_OP_IDLE;
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

    device_capability_snapshot_t committed;
    int persist_index = -1;
    bool changed = false;
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        capability_record_t *record = &s_records[index];
        bool complete = record->staging_active &&
                        record->staging.snapshot_id == message->snapshot_id &&
                        message->total == record->staging_expected &&
                        record->staging.count == record->staging_expected;
        if (complete) {
            changed = !record->has_committed ||
                      !snapshot_content_equal(&record->committed,
                                              &record->staging);
            if (record->has_committed &&
                record->committed.revision == record->staging.revision &&
                changed) {
                ESP_LOGW(TAG,
                         "[%s] CAP_REVISION_MISMATCH revision=%lu "
                         "content_changed=true",
                         device_id, (unsigned long)record->committed.revision);
            }
            record->staging.state = DEVICE_CAP_STATE_READY;
            record->staging.updated_at_ms = now_ms;
            record->committed = record->staging;
            record->has_committed = true;
            record->staging_active = false;
            committed = record->committed;
            persist_index = index;

            ESP_LOGI(TAG,
                     "[%s] CAP_END committed %u capabilities "
                     "(revision=%lu changed=%d)",
                     device_id, (unsigned)committed.count,
                     (unsigned long)committed.revision, (int)changed);
        } else {
            ESP_LOGW(TAG, "[%s] CAP_END incomplete", device_id);
            record->staging_active = false;
            memset(&record->staging, 0, sizeof(record->staging));
            if (!record->has_committed) {
                record->committed.state = DEVICE_CAP_STATE_ERROR;
            }
            record->operation_state = DEVICE_CAP_OP_IDLE;
        }
    }
    unlock_records();

    if (persist_index >= 0) {
        if (!changed) {
            if (lock_records()) {
                int idx = find_record_locked(device_id);
                if (idx >= 0 && s_records[idx].persist_dirty) {
                    esp_err_t err = persist_record(idx, &committed);
                    if (err == ESP_OK) {
                        s_records[idx].persist_dirty = false;
                    }
                }
                unlock_records();
            }
        } else {
            esp_err_t error = persist_record(persist_index, &committed);
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
                ESP_LOGI(TAG, "[%s] persisted capabilities to NVS", device_id);
            }
        }
    }

    /* Commit listener fires outside the capability mutex, after the persist
     * decision, for every successful commit (changed or not). It must only
     * enqueue work — the exposure consumer relies on this contract. */
    if (persist_index >= 0 && s_commit_listener != NULL) {
        s_commit_listener(device_id, committed.revision,
                          s_commit_listener_context);
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
    capability_record_t *record = &s_records[index];

    if (record->operation_state == DEVICE_CAP_OP_RUNNING &&
        s_owner.active &&
        strcmp(s_owner.device_id, device_id) == 0 &&
        s_owner.operation_id == record->operation_id) {
        /* Owner device disconnect during running operation. */
        ESP_LOGW(TAG, "[%s] disconnect during running operation, "
                      "operation_id=%lu",
                 device_id, (unsigned long)record->operation_id);

        record->staging_active = false;
        memset(&record->staging, 0, sizeof(record->staging));

        if (record->operation_kind == DEVICE_CAP_OP_MANUAL) {
            record->refresh_active.state = DEVICE_CAP_REFRESH_IDLE;
            record->refresh_active.generation = 0;
            record->refresh_last_completed.generation =
                record->refresh_active.generation;
            record->refresh_last_completed.result =
                DEVICE_CAP_REFRESH_RESULT_DISCONNECTED;
            record->refresh_last_completed.finished_at_ms =
                esp_timer_get_time() / 1000;
        } else {
            if (!record->has_committed) {
                record->committed.state = DEVICE_CAP_STATE_ERROR;
            }
        }
        record->operation_state = DEVICE_CAP_OP_IDLE;
        s_owner.active = false;
    } else if (record->operation_state == DEVICE_CAP_OP_QUEUED) {
        /* Queued device disconnect — cancel queued operation. */
        ESP_LOGW(TAG, "[%s] disconnect while queued, cancelling", device_id);

        if (record->operation_kind == DEVICE_CAP_OP_MANUAL) {
            record->refresh_active.state = DEVICE_CAP_REFRESH_IDLE;
            record->refresh_active.generation = 0;
            record->refresh_last_completed.result =
                DEVICE_CAP_REFRESH_RESULT_DISCONNECTED;
            record->refresh_last_completed.finished_at_ms =
                esp_timer_get_time() / 1000;
        } else {
            if (!record->has_committed) {
                record->committed.state = DEVICE_CAP_STATE_ERROR;
            }
        }
        record->operation_state = DEVICE_CAP_OP_IDLE;
    }
    /* If no capability operation, committed snapshot stays unchanged. */

    unlock_records();
    start_next_pending();
}

/* ── Completion handling ────────────────────────────────────────────── */

static void handle_completion(const char *device_id,
                              uint32_t event_op_id,
                              uint32_t event_generation,
                              device_cap_submit_result_t completion)
{
    if (!lock_records()) return;
    int index = find_record_locked(device_id);
    if (index < 0) {
        s_owner.active = false;
        unlock_records();
        start_next_pending();
        return;
    }
    capability_record_t *record = &s_records[index];

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
        record->committed.state == DEVICE_CAP_STATE_READY &&
        completion == DEVICE_CAP_SUBMIT_OK) {
        /* Snapshot committed before final ACK — normal for capability flow. */
    } else if (completion == DEVICE_CAP_SUBMIT_REJECTED ||
               completion == DEVICE_CAP_SUBMIT_TIMEOUT) {
        if (!record->has_committed) {
            record->committed.state = DEVICE_CAP_STATE_ERROR;
        }
    } else if (completion == DEVICE_CAP_SUBMIT_BUSY) {
        if (!record->has_committed) {
            record->committed.state = DEVICE_CAP_STATE_ERROR;
        }
    } else if (completion != DEVICE_CAP_SUBMIT_OK) {
        if (!record->has_committed) {
            record->committed.state = DEVICE_CAP_STATE_ERROR;
        }
    }

    if (record->operation_kind == DEVICE_CAP_OP_MANUAL) {
        device_cap_refresh_result_t refresh_result;
        switch (completion) {
        case DEVICE_CAP_SUBMIT_OK:
            refresh_result = record->has_committed
                                 ? DEVICE_CAP_REFRESH_RESULT_SUCCESS
                                 : DEVICE_CAP_REFRESH_RESULT_INTERNAL_ERROR;
            break;
        case DEVICE_CAP_SUBMIT_BUSY:
            refresh_result = DEVICE_CAP_REFRESH_RESULT_BUSY;
            break;
        case DEVICE_CAP_SUBMIT_TIMEOUT:
            refresh_result = DEVICE_CAP_REFRESH_RESULT_TIMEOUT;
            break;
        case DEVICE_CAP_SUBMIT_NOT_CONNECTED:
            refresh_result = DEVICE_CAP_REFRESH_RESULT_DISCONNECTED;
            break;
        case DEVICE_CAP_SUBMIT_TRANSPORT_ERROR:
            refresh_result = DEVICE_CAP_REFRESH_RESULT_TRANSPORT_ERROR;
            break;
        case DEVICE_CAP_SUBMIT_REJECTED:
            refresh_result = DEVICE_CAP_REFRESH_RESULT_UNSUPPORTED;
            break;
        default:
            refresh_result = DEVICE_CAP_REFRESH_RESULT_INTERNAL_ERROR;
            break;
        }
        record->refresh_active.state = DEVICE_CAP_REFRESH_IDLE;
        record->refresh_active.generation = 0;
        record->refresh_last_completed.generation = event_generation;
        record->refresh_last_completed.result = refresh_result;
        record->refresh_last_completed.finished_at_ms =
            esp_timer_get_time() / 1000;

        ESP_LOGI(TAG, "[%s] CAP_REFRESH_RESULT gen=%lu result=%s",
                 device_id, (unsigned long)event_generation,
                 device_capabilities_refresh_result_name(refresh_result));
    }

    record->operation_state = DEVICE_CAP_OP_IDLE;
    s_owner.active = false;
    unlock_records();
    start_next_pending();
}

/* ── Worker task ────────────────────────────────────────────────────── */

static void capability_worker(void *arg)
{
    (void)arg;
    capability_event_t event;
    for (;;) {
        if (xQueueReceive(s_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        switch (event.type) {
        case CAP_EVENT_READY: {
            if (!lock_records()) break;
            int index = find_record_locked(event.device_id);
            if (index < 0) {
                unlock_records();
                start_discovery(event.device_id, DEVICE_CAP_OP_INITIAL, 0);
                break;
            }
            capability_record_t *record = &s_records[index];
            if (record->has_committed &&
                record->committed.state == DEVICE_CAP_STATE_READY) {
                ESP_LOGI(TAG, "[%s] CAP_READY_CACHE_HIT", event.device_id);
                unlock_records();
                break;
            }
            if (record->operation_state == DEVICE_CAP_OP_QUEUED ||
                record->operation_state == DEVICE_CAP_OP_RUNNING) {
                ESP_LOGI(TAG, "[%s] CAP_READY_DUP operation in progress",
                         event.device_id);
                unlock_records();
                break;
            }
            if (record->has_committed &&
                (record->committed.state == DEVICE_CAP_STATE_ERROR ||
                 record->committed.state == DEVICE_CAP_STATE_UNSUPPORTED)) {
                ESP_LOGI(TAG, "[%s] CAP_READY_NO_AUTO_RETRY state=%s",
                         event.device_id,
                         device_capabilities_state_name(
                             record->committed.state));
                unlock_records();
                break;
            }
            unlock_records();
            ESP_LOGI(TAG, "[%s] CAP_READY_CACHE_MISS starting discovery",
                     event.device_id);
            start_discovery(event.device_id, DEVICE_CAP_OP_INITIAL, 0);
            break;
        }
        case CAP_EVENT_REFRESH:
            start_discovery(event.device_id, DEVICE_CAP_OP_MANUAL,
                            event.refresh_generation);
            break;
        case CAP_EVENT_DISCONNECT:
            handle_disconnect(event.device_id);
            break;
        case CAP_EVENT_NOTIFY:
            if (strcmp(event.message.type, "capabilities_begin") == 0) {
                handle_begin(event.device_id, &event.message);
            } else if (strcmp(event.message.type, "capability_item") == 0) {
                handle_item(event.device_id, &event.message);
            } else {
                handle_end(event.device_id, &event.message);
            }
            break;
        case CAP_EVENT_COMPLETION:
            handle_completion(event.device_id, event.operation_id,
                              event.refresh_generation, event.completion);
            break;
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t device_capabilities_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    memset(s_records, 0, sizeof(s_records));
    memset(&s_owner, 0, sizeof(s_owner));
    s_next_operation_id = 0;
    s_next_global_generation = 0;

    load_persisted();

    s_queue = xQueueCreate(CAP_EVENT_QUEUE_DEPTH, sizeof(capability_event_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(capability_worker, "device_caps", CAP_WORKER_STACK, NULL,
                    CAP_WORKER_PRIORITY, &s_worker) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    return ESP_OK;
}

void device_capabilities_set_submitter(device_cap_submit_fn submitter)
{
    s_submitter = submitter;
}

esp_err_t device_capabilities_register_commit_listener(
    device_capability_commit_listener_t listener, void *context)
{
    if (!lock_records()) return ESP_ERR_TIMEOUT;
    s_commit_listener = listener;
    s_commit_listener_context = context;
    unlock_records();
    return ESP_OK;
}

static esp_err_t enqueue_device_event(capability_event_type_t type,
                                      const char *device_id,
                                      uint32_t operation_id,
                                      uint32_t refresh_generation)
{
    if (!s_initialized || s_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || device_id[0] == '\0' ||
        strnlen(device_id, GW_MSG_DEVICE_ID_LEN) >= GW_MSG_DEVICE_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    capability_event_t event = {
        .type = type,
        .operation_id = operation_id,
        .refresh_generation = refresh_generation,
    };
    strlcpy(event.device_id, device_id, sizeof(event.device_id));
    return xQueueSend(s_queue, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t device_capabilities_on_ready(const char *device_id)
{
    return enqueue_device_event(CAP_EVENT_READY, device_id, 0, 0);
}

void device_capabilities_on_disconnect(const char *device_id)
{
    enqueue_device_event(CAP_EVENT_DISCONNECT, device_id, 0, 0);
}

bool device_capabilities_on_notify(const char *device_id,
                                   const gw_message_t *message)
{
    if (message == NULL ||
        (strcmp(message->type, "capabilities_begin") != 0 &&
         strcmp(message->type, "capability_item") != 0 &&
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

    capability_event_t event = {
        .type = CAP_EVENT_NOTIFY,
        .operation_id = op_id,
        .message = *message,
    };
    strlcpy(event.device_id, device_id, sizeof(event.device_id));
    if (xQueueSend(s_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "[%s] capability queue full", device_id);
    }
    return true;
}

esp_err_t device_capabilities_refresh(const char *device_id,
                                      uint32_t *out_generation)
{
    if (device_id == NULL || device_id[0] == '\0') return ESP_ERR_INVALID_ARG;

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
    capability_record_t *record = &s_records[index];

    if (record->operation_state == DEVICE_CAP_OP_QUEUED ||
        record->operation_state == DEVICE_CAP_OP_RUNNING) {
        unlock_records();
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t gen = next_generation();
    uint32_t op_id = next_operation_id();

    record->operation_kind = DEVICE_CAP_OP_MANUAL;
    record->operation_state = DEVICE_CAP_OP_QUEUED;
    record->operation_id = op_id;
    record->refresh_active.generation = gen;
    record->refresh_active.state = DEVICE_CAP_REFRESH_QUEUED;

    if (out_generation != NULL) *out_generation = gen;

    ESP_LOGI(TAG, "[%s] CAP_REFRESH_RESERVE gen=%lu op_id=%lu",
             device_id, (unsigned long)gen, (unsigned long)op_id);

    unlock_records();

    esp_err_t err = enqueue_device_event(CAP_EVENT_REFRESH, device_id,
                                         op_id, gen);
    if (err != ESP_OK) {
        if (lock_records()) {
            int idx = find_record_locked(device_id);
            if (idx >= 0) {
                s_records[idx].operation_state = DEVICE_CAP_OP_IDLE;
                s_records[idx].refresh_active.state = DEVICE_CAP_REFRESH_IDLE;
                s_records[idx].refresh_active.generation = 0;
            }
            unlock_records();
        }
        return err;
    }

    return ESP_OK;
}

esp_err_t device_capabilities_get(const char *device_id,
                                  device_capability_snapshot_t *out_snapshot)
{
    if (device_id == NULL || out_snapshot == NULL) return ESP_ERR_INVALID_ARG;
    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    strlcpy(out_snapshot->device_id, device_id,
            sizeof(out_snapshot->device_id));
    out_snapshot->state = DEVICE_CAP_STATE_UNKNOWN;
    if (!lock_records()) return ESP_ERR_TIMEOUT;
    int index = find_record_locked(device_id);
    if (index >= 0) {
        *out_snapshot = s_records[index].committed;
        out_snapshot->has_committed = s_records[index].has_committed;
    }
    unlock_records();
    return ESP_OK;
}

esp_err_t device_capabilities_get_refresh_status(
    const char *device_id,
    device_cap_refresh_active_t *out_active,
    device_cap_refresh_completed_t *out_completed)
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

device_cap_validation_t device_capabilities_validate_command(
    const gw_message_t *message, device_capability_t *out_capability)
{
    if (message == NULL || !message->has_device_id ||
        message->command[0] == '\0') {
        return DEVICE_CAP_VALID_ARGUMENT;
    }
    if (strcmp(message->command, DEVICE_CAP_RESERVED_COMMAND) == 0) {
        return DEVICE_CAP_VALID;
    }
    if (!lock_records()) return DEVICE_CAP_VALID_UNKNOWN;
    int index = find_record_locked(message->device_id);
    if (index < 0 || !s_records[index].has_committed ||
        (s_records[index].committed.state != DEVICE_CAP_STATE_READY &&
         s_records[index].committed.state != DEVICE_CAP_STATE_DISCOVERING)) {
        unlock_records();
        return DEVICE_CAP_VALID_UNKNOWN;
    }

    const device_capability_t *capability = NULL;
    for (size_t i = 0; i < s_records[index].committed.count; i++) {
        if (strcmp(s_records[index].committed.items[i].command,
                   message->command) == 0) {
            capability = &s_records[index].committed.items[i];
            break;
        }
    }
    if (capability == NULL) {
        unlock_records();
        return DEVICE_CAP_VALID_UNSUPPORTED_COMMAND;
    }

    bool valid_argument = false;
    switch (capability->value_type) {
    case DEVICE_CAP_VALUE_NONE:
        valid_argument = !message->has_int_value && !message->has_bool_value;
        break;
    case DEVICE_CAP_VALUE_BOOL:
        valid_argument = message->has_bool_value && !message->has_int_value;
        break;
    case DEVICE_CAP_VALUE_INT: {
        int64_t delta = (int64_t)message->int_value -
                        (int64_t)capability->min_value;
        valid_argument = message->has_int_value && !message->has_bool_value &&
                         message->int_value >= capability->min_value &&
                         message->int_value <= capability->max_value &&
                         ((uint64_t)delta % capability->step == 0);
        break;
    }
    }
    if (valid_argument && out_capability != NULL) {
        *out_capability = *capability;
    }
    unlock_records();
    return valid_argument ? DEVICE_CAP_VALID : DEVICE_CAP_VALID_ARGUMENT;
}

esp_err_t device_capabilities_forget(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return ESP_ERR_INVALID_ARG;

    if (!lock_records()) return ESP_ERR_TIMEOUT;
    int index = find_record_locked(device_id);
    if (index < 0) {
        unlock_records();
        return ESP_OK;
    }

    capability_record_t *record = &s_records[index];
    if (record->operation_state == DEVICE_CAP_OP_QUEUED ||
        record->operation_state == DEVICE_CAP_OP_RUNNING) {
        if (s_owner.active &&
            strcmp(s_owner.device_id, device_id) == 0 &&
            s_owner.operation_id == record->operation_id) {
            s_owner.active = false;
        }
        record->operation_state = DEVICE_CAP_OP_IDLE;
    }
    record->staging_active = false;
    memset(&record->staging, 0, sizeof(record->staging));
    unlock_records();

    nvs_handle_t handle;
    esp_err_t error = nvs_open(CAP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    char key[8];
    capability_nvs_key(index, key);
    error = nvs_erase_key(handle, key);
    if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);

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

const char *device_capabilities_state_name(device_cap_state_t state)
{
    switch (state) {
    case DEVICE_CAP_STATE_UNKNOWN: return "unknown";
    case DEVICE_CAP_STATE_DISCOVERING: return "discovering";
    case DEVICE_CAP_STATE_READY: return "ready";
    case DEVICE_CAP_STATE_UNSUPPORTED: return "unsupported";
    case DEVICE_CAP_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

const char *device_capabilities_refresh_result_name(
    device_cap_refresh_result_t result)
{
    switch (result) {
    case DEVICE_CAP_REFRESH_RESULT_NONE: return "none";
    case DEVICE_CAP_REFRESH_RESULT_SUCCESS: return "success";
    case DEVICE_CAP_REFRESH_RESULT_UNCHANGED: return "unchanged";
    case DEVICE_CAP_REFRESH_RESULT_NOT_PERSISTED: return "not_persisted";
    case DEVICE_CAP_REFRESH_RESULT_UNSUPPORTED: return "unsupported";
    case DEVICE_CAP_REFRESH_RESULT_BUSY: return "busy";
    case DEVICE_CAP_REFRESH_RESULT_TIMEOUT: return "timeout";
    case DEVICE_CAP_REFRESH_RESULT_DISCONNECTED: return "disconnected";
    case DEVICE_CAP_REFRESH_RESULT_TRANSPORT_ERROR: return "transport_error";
    case DEVICE_CAP_REFRESH_RESULT_PROTOCOL_ERROR: return "protocol_error";
    case DEVICE_CAP_REFRESH_RESULT_INTERNAL_ERROR: return "internal_error";
    default: return "unknown";
    }
}

void device_capabilities_reset_for_test(void)
{
    if (lock_records()) {
        memset(s_records, 0, sizeof(s_records));
        memset(&s_owner, 0, sizeof(s_owner));
        s_next_operation_id = 0;
        s_next_global_generation = 0;
        unlock_records();
    }
}
