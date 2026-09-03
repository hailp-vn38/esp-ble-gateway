#include "device_schema_internal.h"

#include <stddef.h>
#include <string.h>

#include "device_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "memory_policy.h"

static const char *TAG = "schema";

/* ── Runtime context (singleton) ────────────────────────────────────── */

typedef struct {
    schema_record_t *records;
    SemaphoreHandle_t mutex;

    device_schema_submit_fn submitter;

    device_schema_commit_listener_t commit_listener;
    void *commit_listener_context;
    device_schema_commit_listener2_t commit_listener2;
    void *commit_listener2_context;

    schema_global_owner_t owner;

    uint32_t next_operation_id;
    uint32_t next_generation;

    bool initialized;
} schema_runtime_t;

static schema_runtime_t s_runtime;

/* ── Lock / unlock ──────────────────────────────────────────────────── */

bool schema_runtime_lock(void)
{
    return s_runtime.mutex != NULL &&
           xSemaphoreTake(s_runtime.mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

void schema_runtime_unlock(void)
{
    xSemaphoreGive(s_runtime.mutex);
}

/* ── Record lookup (caller must hold lock) ──────────────────────────── */

schema_record_t *schema_runtime_find_locked(const char *device_id)
{
    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (s_runtime.records[i].used &&
            strcmp(s_runtime.records[i].committed.device_id, device_id) == 0) {
            return &s_runtime.records[i];
        }
    }
    return NULL;
}

schema_record_t *schema_runtime_find_or_create_locked(const char *device_id)
{
    schema_record_t *existing = schema_runtime_find_locked(device_id);
    if (existing != NULL) return existing;

    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (s_runtime.records[i].used) continue;
        memset(&s_runtime.records[i], 0, sizeof(s_runtime.records[i]));
        s_runtime.records[i].used = true;
        strlcpy(s_runtime.records[i].committed.device_id, device_id,
                sizeof(s_runtime.records[i].committed.device_id));
        s_runtime.records[i].committed.state = DEVICE_SCHEMA_STATE_UNKNOWN;
        return &s_runtime.records[i];
    }
    return NULL;
}

/* ── ID generators ──────────────────────────────────────────────────── */

uint32_t schema_runtime_next_operation_id(void)
{
    uint32_t id;
    do {
        id = ++s_runtime.next_operation_id;
    } while (id == 0);
    return id;
}

uint32_t schema_runtime_next_generation(void)
{
    uint32_t gen;
    do {
        gen = ++s_runtime.next_generation;
    } while (gen == 0);
    return gen;
}

/* ── Submitter access ───────────────────────────────────────────────── */

device_schema_submit_fn schema_runtime_get_submitter(void)
{
    return s_runtime.submitter;
}

/* ── Owner access ───────────────────────────────────────────────────── */

schema_global_owner_t *schema_runtime_owner(void)
{
    return &s_runtime.owner;
}

/* ── Commit notification ────────────────────────────────────────────── */

void schema_runtime_notify_commit(const char *device_id, uint32_t revision)
{
    if (s_runtime.commit_listener != NULL) {
        s_runtime.commit_listener(device_id, revision,
                                  s_runtime.commit_listener_context);
    }
    if (s_runtime.commit_listener2 != NULL) {
        s_runtime.commit_listener2(device_id, revision,
                                   s_runtime.commit_listener2_context);
    }
}

/* ── Records accessor ───────────────────────────────────────────────── */

schema_record_t *schema_runtime_records(void)
{
    return s_runtime.records;
}

/* ── Init / reset ───────────────────────────────────────────────────── */

bool schema_runtime_is_initialized(void)
{
    return s_runtime.initialized;
}

void schema_runtime_set_initialized(bool v)
{
    s_runtime.initialized = v;
}

esp_err_t schema_runtime_init(void)
{
    if (s_runtime.mutex == NULL) {
        s_runtime.mutex = xSemaphoreCreateMutex();
        if (s_runtime.mutex == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_runtime.records == NULL) {
        s_runtime.records = gw_mem_calloc(DEVICE_STORE_MAX_DEVICES,
                                          sizeof(*s_runtime.records),
                                          GW_MEM_EXTERNAL_PREFERRED);
        if (s_runtime.records == NULL) return ESP_ERR_NO_MEM;
    }
    memset(s_runtime.records, 0,
           DEVICE_STORE_MAX_DEVICES * sizeof(*s_runtime.records));
    memset(&s_runtime.owner, 0, sizeof(s_runtime.owner));
    s_runtime.commit_listener2 = NULL;
    s_runtime.commit_listener2_context = NULL;
    s_runtime.next_operation_id = 0;
    s_runtime.next_generation = 0;

    schema_load_persisted(s_runtime.records);
    schema_cleanup_legacy_caps();

    return ESP_OK;
}

void schema_runtime_set_submitter(device_schema_submit_fn submitter)
{
    s_runtime.submitter = submitter;
}

void schema_runtime_set_commit_listener(device_schema_commit_listener_t listener,
                                        void *context)
{
    s_runtime.commit_listener = listener;
    s_runtime.commit_listener_context = context;
}

void schema_runtime_set_commit_listener2(device_schema_commit_listener2_t listener,
                                         void *context)
{
    s_runtime.commit_listener2 = listener;
    s_runtime.commit_listener2_context = context;
}

void schema_runtime_reset(void)
{
    if (s_runtime.records != NULL) {
        memset(s_runtime.records, 0,
               DEVICE_STORE_MAX_DEVICES * sizeof(*s_runtime.records));
    }
    memset(&s_runtime.owner, 0, sizeof(s_runtime.owner));
    s_runtime.next_operation_id = 0;
    s_runtime.next_generation = 0;
    s_runtime.initialized = false;
}
