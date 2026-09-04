#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#ifndef CONFIG_MCP_DYNAMIC_ALLOW_DESTRUCTIVE
#define CONFIG_MCP_DYNAMIC_ALLOW_DESTRUCTIVE 0
#endif

#include "device_store.h"
#include "device_schema.h"
#include "device_template.h"
#include "memory_policy.h"
#include "mcp_tool_exposure.h"
#include "mcp_tool_exposure_internal.h"

static const char *TAG = "mcp_exposure";

#ifndef CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED
#define CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED 32
#endif
#ifndef CONFIG_MCP_EXPOSURE_RECORD_MAX
#define CONFIG_MCP_EXPOSURE_RECORD_MAX 96
#endif

/* ---------- Worker event types ---------- */

typedef enum {
    WORKER_EVENT_CAP_COMMITTED = 0,
    WORKER_EVENT_DEVICE_REVOKE,
    WORKER_EVENT_DIRTY_RETRY,
} worker_event_type_t;

typedef struct {
    worker_event_type_t type;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t capability_revision;
} worker_event_t;

/* ---------- State ---------- */

static SemaphoreHandle_t s_mutex = NULL;
static QueueHandle_t s_worker_queue = NULL;
static TaskHandle_t s_worker_task = NULL;
static bool s_initialized = false;
static bool s_dirty = false;

/* Enabled set (RAM): tools currently in the executable catalog. Both tables
 * are runtime-allocated so memory_policy places them in PSRAM; the mutex,
 * queue and task handles stay internal (Plan v1.1 §11). */
static size_t s_enabled_count = 0;
typedef struct {
    char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    uint8_t capability_digest[MCP_CAPABILITY_DIGEST_LEN];
} enabled_entry_t;
static enabled_entry_t *s_enabled;

static mcp_exposure_persisted_record_t *s_persisted;
static size_t s_persisted_count = 0;

/* ---------- Helpers ---------- */

static SemaphoreHandle_t ensure_mutex(void)
{
    if (s_mutex != NULL) return s_mutex;
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex;
}

static size_t find_persisted(const char *device_id, const char *command)
{
    for (size_t i = 0; i < s_persisted_count; i++) {
        if (strcmp(s_persisted[i].device_id, device_id) == 0 &&
            strcmp(s_persisted[i].command, command) == 0) {
            return i;
        }
    }
    return s_persisted_count;
}

static size_t find_persisted_by_feature(const char *device_id,
                                         const char *feature_id)
{
    if (feature_id == NULL || feature_id[0] == '\0') return s_persisted_count;
    for (size_t i = 0; i < s_persisted_count; i++) {
        if (strcmp(s_persisted[i].device_id, device_id) == 0 &&
            strcmp(s_persisted[i].feature_id, feature_id) == 0) {
            return i;
        }
    }
    return s_persisted_count;
}

static esp_err_t persist_save_locked(void)
{
    esp_err_t err = mcp_exposure_store_save(
        s_persisted, s_persisted_count, 0);
    if (err != ESP_OK) {
        s_dirty = true;
        ESP_LOGE(TAG, "Persist failed: %s", esp_err_to_name(err));
    } else {
        s_dirty = false;
    }
    return err;
}

static void remove_enabled_locked(size_t idx)
{
    for (size_t i = idx; i + 1 < s_enabled_count; i++) {
        memcpy(&s_enabled[i], &s_enabled[i + 1], sizeof(enabled_entry_t));
    }
    s_enabled_count--;
    memset(&s_enabled[s_enabled_count], 0, sizeof(enabled_entry_t));
}

/* ---------- Capability commit listener ---------- */

static void on_capability_committed(const char *device_id, uint32_t revision,
                                   void *context)
{
    (void)context;
    worker_event_t ev = {
        .type = WORKER_EVENT_CAP_COMMITTED,
        .capability_revision = revision,
    };
    strlcpy(ev.device_id, device_id, sizeof(ev.device_id));
    if (s_worker_queue != NULL) {
        xQueueSend(s_worker_queue, &ev, 0);
    }
}

/* ---------- Reconcile logic ---------- */

static void reconcile_device(const char *device_id)
{
    device_schema_snapshot_t cap;
    esp_err_t cap_err = device_schema_get(device_id, &cap);

    /* Iterate persisted records for this device. */
    for (size_t i = s_persisted_count; i > 0; ) {
        i--;
        mcp_exposure_persisted_record_t *rec = &s_persisted[i];
        if (strcmp(rec->device_id, device_id) != 0) continue;

        if (cap_err != ESP_OK || !cap.has_committed || cap.tool_count == 0) {
            /* Device or capability missing. */
            if (rec->state == MCP_EXPOSURE_ENABLED) {
                rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
                rec->reason = MCP_EXPOSURE_REASON_DEVICE_MISSING;
            }
            continue;
        }

        /* Find matching capability command. */
        bool found = false;
        for (size_t c = 0; c < cap.tool_count; c++) {
            if (strcmp(cap.tools[c].command, rec->command) == 0) {
                found = true;
                uint8_t digest[MCP_CAPABILITY_DIGEST_LEN];
                mcp_tool_digest_compute(&cap.tools[c], digest);

                if (!mcp_tool_digest_match(digest, rec->capability_digest)) {
                    /* Semantic digest changed. */
                    if (rec->state == MCP_EXPOSURE_ENABLED) {
                        rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
                        rec->reason = MCP_EXPOSURE_REASON_CAPABILITY_CHANGED;
                    }
                }
                memcpy(rec->capability_digest, digest, MCP_CAPABILITY_DIGEST_LEN);
                break;
            }
        }

        if (!found) {
            if (rec->state == MCP_EXPOSURE_ENABLED) {
                rec->state = MCP_EXPOSURE_ORPHANED;
                rec->reason = MCP_EXPOSURE_REASON_COMMAND_MISSING;
            }
        }

        /* Re-enable if ORPHANED command returns. */
        if (found && rec->state == MCP_EXPOSURE_ORPHANED) {
            rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
            rec->reason = MCP_EXPOSURE_REASON_NONE;
        }
    }

    /* ── Semantic auto-expose: create records for features with templates ── */
    if (cap_err == ESP_OK && cap.has_committed && cap.feature_count > 0) {
        for (size_t f = 0; f < cap.feature_count; f++) {
            const device_schema_feature_t *feat = &cap.features[f];
            if (feat->feature_id[0] == '\0') continue;

            const device_template_t *tpl = device_template_resolve(
                feat->feature_type, feat->feature_schema_version);
            if (tpl == NULL) continue;

            /* Find the write command for this feature. */
            if (feat->writable_tool_index < 0 ||
                (size_t)feat->writable_tool_index >= cap.tool_count) continue;
            const char *write_cmd =
                cap.tools[feat->writable_tool_index].command;

            /* Find or create a persisted record for this feature. */
            size_t idx = find_persisted_by_feature(device_id, feat->feature_id);
            uint8_t digest[MCP_CAPABILITY_DIGEST_LEN];
            mcp_tool_digest_compute(&cap.tools[feat->writable_tool_index],
                                    digest);

            if (idx >= s_persisted_count) {
                /* New semantic tool — create persisted record. */
                if (s_persisted_count >= CONFIG_MCP_EXPOSURE_RECORD_MAX) {
                    ESP_LOGW(TAG, "Cannot auto-expose: record capacity full");
                    continue;
                }
                idx = s_persisted_count;
                mcp_exposure_persisted_record_t *rec = &s_persisted[idx];
                memset(rec, 0, sizeof(*rec));
                strlcpy(rec->device_id, device_id, sizeof(rec->device_id));
                strlcpy(rec->command, write_cmd, sizeof(rec->command));
                strlcpy(rec->feature_id, feat->feature_id,
                        sizeof(rec->feature_id));
                rec->state = MCP_EXPOSURE_ENABLED;
                rec->reason = MCP_EXPOSURE_REASON_NONE;
                rec->naming_version = MCP_EXP_NAMING_VERSION;
                rec->flags = 0;
                memcpy(rec->capability_digest, digest,
                       MCP_CAPABILITY_DIGEST_LEN);
                s_persisted_count++;
            } else {
                /* Existing record — migrate naming_version if needed. */
                mcp_exposure_persisted_record_t *rec = &s_persisted[idx];
                if (rec->naming_version < MCP_EXP_NAMING_VERSION) {
                    strlcpy(rec->command, write_cmd, sizeof(rec->command));
                    rec->naming_version = MCP_EXP_NAMING_VERSION;
                }
                /* A digest change requires explicit review. Never promote a
                 * persisted NEEDS_REVIEW/ORPHANED record during reconcile. */
                if (!mcp_tool_digest_match(digest,
                                           rec->capability_digest)) {
                    rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
                    rec->reason = MCP_EXPOSURE_REASON_CAPABILITY_CHANGED;
                }
                memcpy(rec->capability_digest, digest,
                       MCP_CAPABILITY_DIGEST_LEN);
            }

            /* Mark raw command as feature-bound. */
            for (size_t r = 0; r < s_persisted_count; r++) {
                if (strcmp(s_persisted[r].device_id, device_id) == 0 &&
                    strcmp(s_persisted[r].command, write_cmd) == 0 &&
                    s_persisted[r].feature_id[0] == '\0') {
                    s_persisted[r].flags |= MCP_EXP_FLAG_FEATURE_BOUND;
                    break;
                }
            }
        }
    }

    /* ── Unbind stale feature records ── */
    for (size_t i = s_persisted_count; i > 0; ) {
        i--;
        mcp_exposure_persisted_record_t *rec = &s_persisted[i];
        if (strcmp(rec->device_id, device_id) != 0) continue;
        if (rec->feature_id[0] == '\0') continue;

        /* Check if this feature still exists in the schema. */
        bool feature_exists = false;
        if (cap_err == ESP_OK && cap.has_committed) {
            for (size_t f = 0; f < cap.feature_count; f++) {
                if (strcmp(cap.features[f].feature_id, rec->feature_id) == 0) {
                    feature_exists = true;
                    break;
                }
            }
        }

        if (!feature_exists) {
            /* Feature removed — remove persisted record. */
            for (size_t j = i; j + 1 < s_persisted_count; j++) {
                memcpy(&s_persisted[j], &s_persisted[j + 1],
                       sizeof(mcp_exposure_persisted_record_t));
            }
            s_persisted_count--;
        }
    }
}

/* ---------- Worker task ---------- */

static void process_event(const worker_event_t *ev)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    switch (ev->type) {
    case WORKER_EVENT_CAP_COMMITTED:
        reconcile_device(ev->device_id);
        persist_save_locked();
        break;

    case WORKER_EVENT_DEVICE_REVOKE:
        /* Remove all enabled tools and persisted records for this device. */
        for (size_t i = s_enabled_count; i > 0; ) {
            i--;
            if (strcmp(s_enabled[i].device_id, ev->device_id) == 0) {
                remove_enabled_locked(i);
            }
        }
        for (size_t i = s_persisted_count; i > 0; ) {
            i--;
            if (strcmp(s_persisted[i].device_id, ev->device_id) == 0) {
                for (size_t j = i; j + 1 < s_persisted_count; j++) {
                    memcpy(&s_persisted[j], &s_persisted[j + 1],
                           sizeof(mcp_exposure_persisted_record_t));
                }
                s_persisted_count--;
            }
        }
        persist_save_locked();
        break;

    case WORKER_EVENT_DIRTY_RETRY:
        if (s_dirty) {
            persist_save_locked();
        }
        break;
    }

    xSemaphoreGive(s_mutex);
}

static void worker_task(void *arg)
{
    (void)arg;
    worker_event_t ev;
    while (true) {
        if (xQueueReceive(s_worker_queue, &ev, portMAX_DELAY) == pdTRUE) {
            process_event(&ev);
        }
    }
}

/* ---------- Boot reconciliation ---------- */

static void boot_reconcile(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Schema migration v2 → v3: migrate old records that lack feature_id. */
    for (size_t i = 0; i < s_persisted_count; i++) {
        mcp_exposure_persisted_record_t *rec = &s_persisted[i];
        if (rec->naming_version >= MCP_EXP_NAMING_VERSION) continue;

        /* Check if this command is now bound to a feature. */
        device_schema_snapshot_t cap;
        if (device_schema_get(rec->device_id, &cap) != ESP_OK ||
            !cap.has_committed) continue;

        for (size_t f = 0; f < cap.feature_count; f++) {
            const device_schema_feature_t *feat = &cap.features[f];
            if (feat->feature_id[0] == '\0') continue;
            if (feat->writable_tool_index < 0 ||
                (size_t)feat->writable_tool_index >= cap.tool_count) continue;
            if (strcmp(cap.tools[feat->writable_tool_index].command,
                       rec->command) != 0) continue;

            /* This command maps to a feature — migrate the record. */
            strlcpy(rec->feature_id, feat->feature_id,
                    sizeof(rec->feature_id));
            rec->naming_version = MCP_EXP_NAMING_VERSION;
            rec->flags |= MCP_EXP_FLAG_FEATURE_BOUND;
            break;
        }
        if (rec->feature_id[0] == '\0') {
            rec->naming_version = MCP_EXP_NAMING_VERSION;
        }
    }

    for (size_t i = 0; i < s_persisted_count; i++) {
        mcp_exposure_persisted_record_t *rec = &s_persisted[i];

        /* Check device exists. */
        device_entry_t entry;
        if (device_store_get(rec->device_id, &entry) != DEVICE_STORE_OK) {
            rec->state = MCP_EXPOSURE_ORPHANED;
            rec->reason = MCP_EXPOSURE_REASON_DEVICE_MISSING;
            continue;
        }

        /* Check capability snapshot exists. */
        device_schema_snapshot_t cap;
        if (device_schema_get(rec->device_id, &cap) != ESP_OK ||
            !cap.has_committed || cap.tool_count == 0) {
            rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
            rec->reason = MCP_EXPOSURE_REASON_DEVICE_MISSING;
            continue;
        }

        /* Check command exists and digest matches. */
        bool cmd_found = false;
        for (size_t c = 0; c < cap.tool_count; c++) {
            if (strcmp(cap.tools[c].command, rec->command) == 0) {
                cmd_found = true;
                uint8_t digest[MCP_CAPABILITY_DIGEST_LEN];
                mcp_tool_digest_compute(&cap.tools[c], digest);

                bool digest_matches =
                    mcp_tool_digest_match(digest, rec->capability_digest);
                if (rec->state == MCP_EXPOSURE_ENABLED && digest_matches) {
                    rec->naming_version = MCP_EXP_NAMING_VERSION;
                } else if (!digest_matches) {
                    /* Digest mismatch. */
                    rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
                    rec->reason = MCP_EXPOSURE_REASON_CAPABILITY_CHANGED;
                    memcpy(rec->capability_digest, digest, MCP_CAPABILITY_DIGEST_LEN);
                }
                break;
            }
        }

        if (!cmd_found) {
            rec->state = MCP_EXPOSURE_ORPHANED;
            rec->reason = MCP_EXPOSURE_REASON_COMMAND_MISSING;
        }
    }

    xSemaphoreGive(s_mutex);
}

/* ---------- Public API ---------- */

esp_err_t mcp_tool_exposure_init(void)
{
    if (s_initialized) return ESP_OK;

    if (ensure_mutex() == NULL) {
        ESP_LOGE(TAG, "Failed to create exposure mutex");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Allocate state tables first so a failure unwinds without leaking
     * partially-created resources (Plan v1.1 §11.4). */
    if (s_enabled == NULL) {
        s_enabled = gw_mem_calloc(CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED,
                                  sizeof(*s_enabled),
                                  GW_MEM_EXTERNAL_PREFERRED);
        if (s_enabled == NULL) {
            xSemaphoreGive(s_mutex);
            ESP_LOGE(TAG, "Failed to allocate enabled table");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_persisted == NULL) {
        s_persisted = gw_mem_calloc(CONFIG_MCP_EXPOSURE_RECORD_MAX,
                                    sizeof(*s_persisted),
                                    GW_MEM_EXTERNAL_PREFERRED);
        if (s_persisted == NULL) {
            gw_mem_free(s_enabled);
            s_enabled = NULL;
            xSemaphoreGive(s_mutex);
            ESP_LOGE(TAG, "Failed to allocate persisted table");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Create worker queue and task. */
    if (s_worker_queue == NULL) {
        s_worker_queue = xQueueCreate(16, sizeof(worker_event_t));
        if (s_worker_queue == NULL) {
            gw_mem_free(s_persisted);
            s_persisted = NULL;
            gw_mem_free(s_enabled);
            s_enabled = NULL;
            xSemaphoreGive(s_mutex);
            ESP_LOGE(TAG, "Failed to create worker queue");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_worker_task == NULL &&
        xTaskCreatePinnedToCore(worker_task, "mcp_exp", 4096, NULL, 5,
                                &s_worker_task, 0) != pdPASS) {
        vQueueDelete(s_worker_queue);
        s_worker_queue = NULL;
        gw_mem_free(s_persisted);
        s_persisted = NULL;
        gw_mem_free(s_enabled);
        s_enabled = NULL;
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Failed to create worker task");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_mutex);

    /* Load persisted exposure catalog. */
    size_t loaded = 0;
    uint32_t revision = 0;
    esp_err_t err = mcp_exposure_store_load(
        s_persisted, CONFIG_MCP_EXPOSURE_RECORD_MAX, &loaded, &revision);
    if (err == ESP_ERR_INVALID_VERSION) {
        /* Schema version mismatch (v2→v3) — clear store, records will be
         * recreated during schema commit reconciliation. */
        ESP_LOGW(TAG, "Exposure store schema outdated, clearing");
        mcp_exposure_store_erase();
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load exposure store: %s",
                 esp_err_to_name(err));
        /* Continue with empty store (fail-closed). */
    }
    s_persisted_count = loaded;

    /* Boot reconcile — rebuild enabled catalog from persisted + capability. */
    boot_reconcile();

    /* Register capability commit listener. */
    err = device_schema_register_commit_listener(
        on_capability_committed, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register commit listener: %s",
                 esp_err_to_name(err));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "MCP tool exposure initialized (%zu records)",
             s_persisted_count);
    return ESP_OK;
}

esp_err_t mcp_tool_exposure_enable(
    const char *device_id,
    const char *command,
    const mcp_exposure_enable_options_t *options)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || command == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Check device exists. */
    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    /* Check committed capability snapshot. */
    device_schema_snapshot_t cap;
    if (device_schema_get(device_id, &cap) != ESP_OK ||
        !cap.has_committed || cap.tool_count == 0) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Cannot enable: no committed capabilities for %s",
                 device_id);
        return ESP_ERR_INVALID_STATE;
    }

    /* Find matching capability. */
    const device_schema_tool_t *target = NULL;
    for (size_t i = 0; i < cap.tool_count; i++) {
        if (strcmp(cap.tools[i].command, command) == 0) {
            target = &cap.tools[i];
            break;
        }
    }
    if (target == NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    /* Destructive policy check. */
    if ((target->flags & DEVICE_SCHEMA_FLAG_DESTRUCTIVE) != 0) {
        if (!CONFIG_MCP_DYNAMIC_ALLOW_DESTRUCTIVE) {
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "Destructive command '%s' blocked by policy",
                     command);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (options == NULL || !options->confirm_destructive) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Check if already enabled. */
    size_t existing = find_persisted(device_id, command);
    if (existing < s_persisted_count &&
        s_persisted[existing].state == MCP_EXPOSURE_ENABLED) {
        xSemaphoreGive(s_mutex);
        return ESP_OK; /* Already enabled. */
    }

    /* Compute digest. */
    uint8_t digest[MCP_CAPABILITY_DIGEST_LEN];
    mcp_tool_digest_compute(target, digest);

    /* Persist-first: save the record. */
    bool had_existing = existing < s_persisted_count;
    mcp_exposure_persisted_record_t previous = {0};
    if (had_existing) {
        previous = s_persisted[existing];
    }
    if (existing < s_persisted_count) {
        /* Update existing record. */
        s_persisted[existing].state = MCP_EXPOSURE_ENABLED;
        s_persisted[existing].reason = MCP_EXPOSURE_REASON_NONE;
        memcpy(s_persisted[existing].capability_digest, digest,
               MCP_CAPABILITY_DIGEST_LEN);
    } else {
        /* Add new record. */
        if (s_persisted_count >= CONFIG_MCP_EXPOSURE_RECORD_MAX) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }
        mcp_exposure_persisted_record_t *rec = &s_persisted[s_persisted_count];
        memset(rec, 0, sizeof(*rec));
        strlcpy(rec->device_id, device_id, sizeof(rec->device_id));
        strlcpy(rec->command, command, sizeof(rec->command));
        rec->state = MCP_EXPOSURE_ENABLED;
        rec->reason = MCP_EXPOSURE_REASON_NONE;
        rec->naming_version = MCP_EXP_NAMING_VERSION;
        rec->flags = 0;
        memcpy(rec->capability_digest, digest, MCP_CAPABILITY_DIGEST_LEN);
        s_persisted_count++;
    }

    esp_err_t err = persist_save_locked();
    if (err != ESP_OK) {
        /* NVS was not committed; restore the in-memory record as well. */
        if (had_existing) {
            s_persisted[existing] = previous;
        } else {
            s_persisted_count--;
            memset(&s_persisted[s_persisted_count], 0,
                   sizeof(s_persisted[s_persisted_count]));
        }
        xSemaphoreGive(s_mutex);
        return err;
    }

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Enabled exposure: %s.%s", device_id, command);
    return ESP_OK;
}

esp_err_t mcp_tool_exposure_disable(const char *device_id,
                                    const char *command)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || command == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Remove persisted record. */
    size_t idx = find_persisted(device_id, command);
    if (idx < s_persisted_count) {
        for (size_t i = idx; i + 1 < s_persisted_count; i++) {
            memcpy(&s_persisted[i], &s_persisted[i + 1],
                   sizeof(mcp_exposure_persisted_record_t));
        }
        s_persisted_count--;
    }

    persist_save_locked();
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Disabled exposure: %s.%s", device_id, command);
    return ESP_OK;
}

esp_err_t mcp_tool_exposure_get(const char *device_id,
                                const char *command,
                                mcp_tool_exposure_t *out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || command == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t idx = find_persisted(device_id, command);
    if (idx >= s_persisted_count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    const mcp_exposure_persisted_record_t *rec = &s_persisted[idx];
    strlcpy(out->device_id, rec->device_id, sizeof(out->device_id));
    strlcpy(out->command, rec->command, sizeof(out->command));
    strlcpy(out->feature_id, rec->feature_id, sizeof(out->feature_id));
    out->feature_bound = (rec->flags & MCP_EXP_FLAG_FEATURE_BOUND) != 0;
    out->state = (mcp_exposure_state_t)rec->state;
    out->reason = (mcp_exposure_reason_t)rec->reason;
    memcpy(out->capability_digest, rec->capability_digest,
           MCP_CAPABILITY_DIGEST_LEN);

    /* Compact-only: no dynamic tool name. */
    out->tool_name[0] = '\0';

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t mcp_tool_exposure_snapshot(mcp_tool_exposure_t *out,
                                     size_t capacity, size_t *out_count)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (out == NULL || out_count == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t n = s_persisted_count < capacity ? s_persisted_count : capacity;
    for (size_t i = 0; i < n; i++) {
        const mcp_exposure_persisted_record_t *rec = &s_persisted[i];
        strlcpy(out[i].device_id, rec->device_id, sizeof(out[i].device_id));
        strlcpy(out[i].command, rec->command, sizeof(out[i].command));
        strlcpy(out[i].feature_id, rec->feature_id, sizeof(out[i].feature_id));
        out[i].feature_bound = (rec->flags & MCP_EXP_FLAG_FEATURE_BOUND) != 0;
        out[i].state = (mcp_exposure_state_t)rec->state;
        out[i].reason = (mcp_exposure_reason_t)rec->reason;
        memcpy(out[i].capability_digest, rec->capability_digest,
               MCP_CAPABILITY_DIGEST_LEN);
        /* Compact-only: no dynamic tool name. */
        out[i].tool_name[0] = '\0';
    }
    *out_count = n;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t mcp_tool_exposure_reconcile_device_async(
    const char *device_id, uint32_t capability_revision)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL) return ESP_ERR_INVALID_ARG;

    worker_event_t ev = {
        .type = WORKER_EVENT_CAP_COMMITTED,
        .capability_revision = capability_revision,
    };
    strlcpy(ev.device_id, device_id, sizeof(ev.device_id));
    if (xQueueSend(s_worker_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Worker queue full, reconcile deferred for %s",
                 device_id);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mcp_tool_exposure_forget_device(const char *device_id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL) return ESP_ERR_INVALID_ARG;

    /* Immediate RAM revoke — must happen before any async work. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (size_t i = s_enabled_count; i > 0; ) {
        i--;
        if (strcmp(s_enabled[i].device_id, device_id) == 0) {
            remove_enabled_locked(i);
        }
    }

    xSemaphoreGive(s_mutex);

    /* Enqueue async persist cleanup. */
    worker_event_t ev = {
        .type = WORKER_EVENT_DEVICE_REVOKE,
    };
    strlcpy(ev.device_id, device_id, sizeof(ev.device_id));
    if (xQueueSend(s_worker_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Worker queue full, device revoke deferred for %s",
                 device_id);
        s_dirty = true;
        /* Trigger a retry so dirty persist runs when queue drains. */
        worker_event_t retry = { .type = WORKER_EVENT_DIRTY_RETRY };
        xQueueSend(s_worker_queue, &retry, 0);
    }

    ESP_LOGI(TAG, "Revoked all tools for device: %s", device_id);
    return ESP_OK;
}

esp_err_t mcp_tool_exposure_refresh_device_name(const char *device_id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || device_id[0] == '\0') return ESP_ERR_INVALID_ARG;
    /* Compact-only: no dynamic tool names to refresh. */
    return ESP_OK;
}

esp_err_t mcp_tool_exposure_get_capacity(mcp_exposure_capacity_t *out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out->enabled = s_enabled_count;
    out->max_enabled = CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED;
    out->records = s_persisted_count;
    out->max_records = CONFIG_MCP_EXPOSURE_RECORD_MAX;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

/* ---------- Feature-bound semantic tool exposure ---------- */

esp_err_t mcp_tool_expose_feature(const char *device_id,
                                   const char *feature_id,
                                   const char *command,
                                   const char *tool_name,
                                   const device_schema_tool_t *cap)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || feature_id == NULL || command == NULL ||
        tool_name == NULL || cap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Check if already exposed for this feature. */
    size_t idx = find_persisted_by_feature(device_id, feature_id);
    if (idx < s_persisted_count &&
        s_persisted[idx].state == MCP_EXPOSURE_ENABLED) {
        xSemaphoreGive(s_mutex);
        return ESP_OK; /* Already enabled. */
    }

    /* Compute digest. */
    uint8_t digest[MCP_CAPABILITY_DIGEST_LEN];
    mcp_tool_digest_compute(cap, digest);

    if (idx < s_persisted_count) {
        /* Update existing record. */
        s_persisted[idx].state = MCP_EXPOSURE_ENABLED;
        s_persisted[idx].reason = MCP_EXPOSURE_REASON_NONE;
        memcpy(s_persisted[idx].capability_digest, digest,
               MCP_CAPABILITY_DIGEST_LEN);
    } else {
        /* Add new record. */
        if (s_persisted_count >= CONFIG_MCP_EXPOSURE_RECORD_MAX) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }
        mcp_exposure_persisted_record_t *rec = &s_persisted[s_persisted_count];
        memset(rec, 0, sizeof(*rec));
        strlcpy(rec->device_id, device_id, sizeof(rec->device_id));
        strlcpy(rec->command, command, sizeof(rec->command));
        strlcpy(rec->feature_id, feature_id, sizeof(rec->feature_id));
        rec->state = MCP_EXPOSURE_ENABLED;
        rec->reason = MCP_EXPOSURE_REASON_NONE;
        rec->naming_version = MCP_EXP_NAMING_VERSION;
        rec->flags = 0;
        memcpy(rec->capability_digest, digest, MCP_CAPABILITY_DIGEST_LEN);
        s_persisted_count++;
    }

    persist_save_locked();

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Exposed feature tool: %s", tool_name);
    return ESP_OK;
}

esp_err_t mcp_tool_unbind_feature(const char *device_id,
                                   const char *feature_id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || feature_id == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t idx = find_persisted_by_feature(device_id, feature_id);
    if (idx >= s_persisted_count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    /* Remove persisted record. */
    for (size_t i = idx; i + 1 < s_persisted_count; i++) {
        memcpy(&s_persisted[i], &s_persisted[i + 1],
               sizeof(mcp_exposure_persisted_record_t));
    }
    s_persisted_count--;

    persist_save_locked();
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Unbound feature tool: %s.%s", device_id, feature_id);
    return ESP_OK;
}

bool mcp_tool_is_feature_bound(const char *device_id, const char *command)
{
    if (!s_initialized || device_id == NULL || command == NULL) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_persisted_count; i++) {
        if (strcmp(s_persisted[i].device_id, device_id) == 0 &&
            strcmp(s_persisted[i].command, command) == 0 &&
            (s_persisted[i].flags & MCP_EXP_FLAG_FEATURE_BOUND) != 0) {
            xSemaphoreGive(s_mutex);
            return true;
        }
    }

    xSemaphoreGive(s_mutex);
    return false;
}

/* ── Feature-level control enable/disable (compact MCP) ──────────────── */

static uint32_t s_policy_revision = 0;

uint32_t mcp_tool_exposure_get_policy_revision(void)
{
    return s_policy_revision;
}

esp_err_t mcp_semantic_control_get_hints(
    const char *device_id,
    mcp_control_hint_t *out,
    size_t capacity,
    size_t *out_count,
    bool *out_truncated)
{
    if (device_id == NULL || out_count == NULL || out_truncated == NULL ||
        (capacity > 0 && out == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    *out_truncated = false;

    device_schema_snapshot_t schema = {0};
    if (device_schema_get(device_id, &schema) != ESP_OK ||
        !schema.has_committed) {
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t i = 0; i < schema.feature_count; ++i) {
        const device_schema_feature_t *feature = &schema.features[i];
        if (feature->feature_id[0] == '\0' ||
            feature->writable_tool_index < 0 ||
            (size_t)feature->writable_tool_index >= schema.tool_count) {
            continue;
        }
        const device_schema_tool_t *tool =
            &schema.tools[feature->writable_tool_index];
        const device_template_t *tpl = device_template_resolve(
            feature->feature_type, feature->feature_schema_version);
        device_template_value_type_t value_type =
            device_template_property_value_type(feature->property_id);
        if (tpl == NULL || tpl->semantic_name[0] == '\0' ||
            (value_type != DEVICE_TEMPLATE_VALUE_BOOL &&
             value_type != DEVICE_TEMPLATE_VALUE_INT) ||
            tool->value_type != (uint8_t)value_type ||
            (tool->flags & DEVICE_SCHEMA_FLAG_DESTRUCTIVE) != 0) {
            continue;
        }

        mcp_tool_exposure_t exposure = {0};
        if (mcp_tool_exposure_get_feature(device_id, feature->feature_id,
                                          &exposure) != ESP_OK ||
            !exposure.control_enabled ||
            exposure.state != MCP_EXPOSURE_ENABLED) {
            continue;
        }
        uint8_t digest[MCP_CAPABILITY_DIGEST_LEN];
        mcp_tool_digest_compute(tool, digest);
        if (!mcp_tool_digest_match(digest, exposure.capability_digest)) {
            continue;
        }

        if (*out_count >= capacity) {
            *out_truncated = true;
            continue;
        }
        mcp_control_hint_t *hint = &out[*out_count];
        memset(hint, 0, sizeof(*hint));
        strlcpy(hint->feature_id, feature->feature_id,
                sizeof(hint->feature_id));
        strlcpy(hint->semantic_name, tpl->semantic_name,
                sizeof(hint->semantic_name));
        strlcpy(hint->property_name,
                device_template_property_name(feature->property_id),
                sizeof(hint->property_name));
        hint->value_type = (uint8_t)value_type;
        if (value_type == DEVICE_TEMPLATE_VALUE_INT) {
            hint->has_min = true;
            hint->min_value = tool->min_value;
            hint->has_max = true;
            hint->max_value = tool->max_value;
            hint->has_step = true;
            hint->step = tool->step;
        }
        (*out_count)++;
    }
    return ESP_OK;
}

esp_err_t mcp_semantic_control_serialize_hints(cJSON *array,
                                               const mcp_control_hint_t *hints,
                                               size_t count)
{
    if (array == NULL || hints == NULL) return ESP_ERR_INVALID_ARG;

    for (size_t i = 0; i < count; ++i) {
        cJSON *control = cJSON_CreateObject();
        if (control == NULL) return ESP_ERR_NO_MEM;

        cJSON_AddStringToObject(control, "feature_id", hints[i].feature_id);
        cJSON_AddStringToObject(control, "semantic_name",
                                hints[i].semantic_name);
        cJSON_AddStringToObject(control, "property", hints[i].property_name);
        cJSON_AddStringToObject(
            control, "value_type",
            hints[i].value_type == DEVICE_TEMPLATE_VALUE_BOOL ? "bool" : "int");
        cJSON_AddBoolToObject(control, "writable", true);

        if (hints[i].has_min)
            cJSON_AddNumberToObject(control, "minimum", hints[i].min_value);
        if (hints[i].has_max)
            cJSON_AddNumberToObject(control, "maximum", hints[i].max_value);
        if (hints[i].has_step)
            cJSON_AddNumberToObject(control, "step", hints[i].step);

        cJSON_AddItemToArray(array, control);
    }
    return ESP_OK;
}

esp_err_t mcp_tool_exposure_get_feature(const char *device_id,
                                         const char *feature_id,
                                         mcp_tool_exposure_t *out)
{
    if (!s_initialized || device_id == NULL || feature_id == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_persisted_count; i++) {
        if (strcmp(s_persisted[i].device_id, device_id) == 0 &&
            s_persisted[i].feature_id[0] != '\0' &&
            strcmp(s_persisted[i].feature_id, feature_id) == 0) {
            memset(out, 0, sizeof(*out));
            strlcpy(out->device_id, s_persisted[i].device_id,
                    sizeof(out->device_id));
            strlcpy(out->command, s_persisted[i].command,
                    sizeof(out->command));
            strlcpy(out->feature_id, s_persisted[i].feature_id,
                    sizeof(out->feature_id));
            out->feature_bound = true;
            out->control_enabled =
                (s_persisted[i].flags & MCP_EXP_FLAG_USER_DISABLED) == 0;
            out->state = s_persisted[i].state;
            out->reason = s_persisted[i].reason;
            memcpy(out->capability_digest, s_persisted[i].capability_digest,
                   sizeof(out->capability_digest));
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t mcp_tool_exposure_set_feature_enabled(const char *device_id,
                                                 const char *feature_id,
                                                 bool enabled)
{
    if (!s_initialized || device_id == NULL || feature_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_persisted_count; i++) {
        if (strcmp(s_persisted[i].device_id, device_id) == 0 &&
            s_persisted[i].feature_id[0] != '\0' &&
            strcmp(s_persisted[i].feature_id, feature_id) == 0) {
            if (enabled) {
                s_persisted[i].flags &= ~MCP_EXP_FLAG_USER_DISABLED;
            } else {
                s_persisted[i].flags |= MCP_EXP_FLAG_USER_DISABLED;
            }
            s_policy_revision++;
            persist_save_locked();
            xSemaphoreGive(s_mutex);

            ESP_LOGI(TAG, "Feature %s.%s control %s (policy rev=%lu)",
                     device_id, feature_id,
                     enabled ? "enabled" : "disabled",
                     (unsigned long)s_policy_revision);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}
