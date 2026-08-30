#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#ifndef CONFIG_MCP_DYNAMIC_ALLOW_DESTRUCTIVE
#define CONFIG_MCP_DYNAMIC_ALLOW_DESTRUCTIVE 0
#endif

#include "device_store.h"
#include "device_capabilities.h"
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

static size_t find_enabled(const char *tool_name)
{
    for (size_t i = 0; i < s_enabled_count; i++) {
        if (strcmp(s_enabled[i].tool_name, tool_name) == 0) return i;
    }
    return s_enabled_count;
}

static esp_err_t generate_tool_name_for_device(const char *device_id,
                                               const char *command,
                                               char *out_name,
                                               size_t out_len)
{
    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    /* A default name copied from the id is not a user-facing device name. */
    if (entry.name[0] == '\0' || strcmp(entry.name, entry.device_id) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return mcp_tool_name_generate(entry.name, command, out_name, out_len);
}

static esp_err_t persist_save_locked(void)
{
    esp_err_t err = mcp_exposure_store_save(
        s_persisted, s_persisted_count,
        mcp_tool_catalog_get_revision());
    if (err != ESP_OK) {
        s_dirty = true;
        ESP_LOGE(TAG, "Persist failed: %s", esp_err_to_name(err));
    } else {
        s_dirty = false;
    }
    return err;
}

static void add_enabled_locked(const char *tool_name, const char *device_id,
                               const char *command,
                               const uint8_t digest[MCP_CAPABILITY_DIGEST_LEN])
{
    if (s_enabled_count >= CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED) return;
    enabled_entry_t *e = &s_enabled[s_enabled_count];
    strlcpy(e->tool_name, tool_name, sizeof(e->tool_name));
    strlcpy(e->device_id, device_id, sizeof(e->device_id));
    strlcpy(e->command, command, sizeof(e->command));
    memcpy(e->capability_digest, digest, MCP_CAPABILITY_DIGEST_LEN);
    s_enabled_count++;
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
    device_capability_snapshot_t cap;
    esp_err_t cap_err = device_capabilities_get(device_id, &cap);

    /* Iterate persisted records for this device. */
    for (size_t i = s_persisted_count; i > 0; ) {
        i--;
        mcp_exposure_persisted_record_t *rec = &s_persisted[i];
        if (strcmp(rec->device_id, device_id) != 0) continue;

        if (cap_err != ESP_OK || !cap.has_committed || cap.count == 0) {
            /* Device or capability missing. */
            if (rec->state == MCP_EXPOSURE_ENABLED) {
                rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
                rec->reason = MCP_EXPOSURE_REASON_DEVICE_MISSING;
            }
            /* Remove from enabled catalog. */
            char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
            if (generate_tool_name_for_device(device_id, rec->command,
                                              tool_name,
                                              sizeof(tool_name)) == ESP_OK) {
                mcp_tool_catalog_remove(tool_name);
            }
            continue;
        }

        /* Find matching capability command. */
        bool found = false;
        for (size_t c = 0; c < cap.count; c++) {
            if (strcmp(cap.items[c].command, rec->command) == 0) {
                found = true;
                uint8_t digest[MCP_CAPABILITY_DIGEST_LEN];
                mcp_tool_digest_compute(&cap.items[c], digest);

                if (!mcp_tool_digest_match(digest, rec->capability_digest)) {
                    /* Semantic digest changed. */
                    if (rec->state == MCP_EXPOSURE_ENABLED) {
                        rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
                        rec->reason = MCP_EXPOSURE_REASON_CAPABILITY_CHANGED;
                        char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
                        if (generate_tool_name_for_device(
                                device_id, rec->command, tool_name,
                                sizeof(tool_name)) == ESP_OK) {
                            mcp_tool_catalog_remove(tool_name);
                        }
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
                char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
                if (generate_tool_name_for_device(device_id, rec->command,
                                                  tool_name,
                                                  sizeof(tool_name)) == ESP_OK) {
                    mcp_tool_catalog_remove(tool_name);
                }
            }
        }

        /* Re-enable if ORPHANED command returns. */
        if (found && rec->state == MCP_EXPOSURE_ORPHANED) {
            rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
            rec->reason = MCP_EXPOSURE_REASON_NONE;
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
        mcp_tool_catalog_remove_device(ev->device_id);
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
        device_capability_snapshot_t cap;
        if (device_capabilities_get(rec->device_id, &cap) != ESP_OK ||
            !cap.has_committed || cap.count == 0) {
            rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
            rec->reason = MCP_EXPOSURE_REASON_DEVICE_MISSING;
            continue;
        }

        /* Check command exists and digest matches. */
        bool cmd_found = false;
        for (size_t c = 0; c < cap.count; c++) {
            if (strcmp(cap.items[c].command, rec->command) == 0) {
                cmd_found = true;
                uint8_t digest[MCP_CAPABILITY_DIGEST_LEN];
                mcp_tool_digest_compute(&cap.items[c], digest);

                bool digest_matches =
                    mcp_tool_digest_match(digest, rec->capability_digest);
                if (rec->state == MCP_EXPOSURE_ENABLED && digest_matches) {
                    /* All good — add to enabled catalog. */
                    char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
                    if (generate_tool_name_for_device(
                            rec->device_id, rec->command, tool_name,
                            sizeof(tool_name)) == ESP_OK) {
                        mcp_tool_binding_t binding = {0};
                        strlcpy(binding.tool_name, tool_name, sizeof(binding.tool_name));
                        strlcpy(binding.device_id, rec->device_id, sizeof(binding.device_id));
                        strlcpy(binding.command, rec->command, sizeof(binding.command));
                        memcpy(&binding.capability, &cap.items[c],
                               sizeof(device_capability_t));
                        if (mcp_tool_catalog_add(&binding) == ESP_OK) {
                            add_enabled_locked(tool_name, rec->device_id,
                                               rec->command,
                                               rec->capability_digest);
                            rec->naming_version = MCP_EXP_NAMING_VERSION;
                        } else {
                            rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
                            rec->reason = MCP_EXPOSURE_REASON_POLICY_BLOCKED;
                        }
                    } else {
                        rec->state = MCP_EXPOSURE_NEEDS_REVIEW;
                        rec->reason = MCP_EXPOSURE_REASON_POLICY_BLOCKED;
                    }
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
    if (err != ESP_OK && err != ESP_ERR_INVALID_VERSION) {
        ESP_LOGE(TAG, "Failed to load exposure store: %s",
                 esp_err_to_name(err));
        /* Continue with empty store (fail-closed). */
    }
    s_persisted_count = loaded;

    /* Boot reconcile — rebuild enabled catalog from persisted + capability. */
    boot_reconcile();

    /* Register capability commit listener. */
    err = device_capabilities_register_commit_listener(
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
    device_capability_snapshot_t cap;
    if (device_capabilities_get(device_id, &cap) != ESP_OK ||
        !cap.has_committed || cap.count == 0) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Cannot enable: no committed capabilities for %s",
                 device_id);
        return ESP_ERR_INVALID_STATE;
    }

    /* Find matching capability. */
    const device_capability_t *target = NULL;
    for (size_t i = 0; i < cap.count; i++) {
        if (strcmp(cap.items[i].command, command) == 0) {
            target = &cap.items[i];
            break;
        }
    }
    if (target == NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    /* Destructive policy check. */
    if ((target->flags & DEVICE_CAP_FLAG_DESTRUCTIVE) != 0) {
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

    /* Generate tool name. */
    char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
    esp_err_t err = generate_tool_name_for_device(
        device_id, command, tool_name, sizeof(tool_name));
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    /* Check if already enabled. */
    size_t existing = find_persisted(device_id, command);
    if (existing < s_persisted_count &&
        s_persisted[existing].state == MCP_EXPOSURE_ENABLED) {
        xSemaphoreGive(s_mutex);
        return ESP_OK; /* Already enabled. */
    }

    /* Check enabled capacity. */
    if (s_enabled_count >= CONFIG_MCP_DYNAMIC_TOOL_MAX_ENABLED) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Compute digest. */
    uint8_t digest[MCP_CAPABILITY_DIGEST_LEN];
    mcp_tool_digest_compute(target, digest);

    /* Persist-first: save the record. */
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
        strlcpy(rec->device_id, device_id, sizeof(rec->device_id));
        strlcpy(rec->command, command, sizeof(rec->command));
        rec->state = MCP_EXPOSURE_ENABLED;
        rec->reason = MCP_EXPOSURE_REASON_NONE;
        rec->naming_version = MCP_EXP_NAMING_VERSION;
        rec->reserved = 0;
        memcpy(rec->capability_digest, digest, MCP_CAPABILITY_DIGEST_LEN);
        s_persisted_count++;
    }

    err = persist_save_locked();
    if (err != ESP_OK) {
        /* Rollback persisted state but do not resurrect on revoke. */
        xSemaphoreGive(s_mutex);
        return err;
    }

    /* Publish: add to enabled catalog. */
    add_enabled_locked(tool_name, device_id, command, digest);

    mcp_tool_binding_t binding = {0};
    strlcpy(binding.tool_name, tool_name, sizeof(binding.tool_name));
    strlcpy(binding.device_id, device_id, sizeof(binding.device_id));
    strlcpy(binding.command, command, sizeof(binding.command));
    memcpy(&binding.capability, target, sizeof(device_capability_t));
    mcp_tool_catalog_add(&binding);

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Enabled tool: %s", tool_name);
    return ESP_OK;
}

esp_err_t mcp_tool_exposure_disable(const char *device_id,
                                    const char *command)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || command == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Remove from enabled catalog immediately (revoke-first). */
    char tool_name[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
    esp_err_t err = generate_tool_name_for_device(
        device_id, command, tool_name, sizeof(tool_name));
    if (err == ESP_OK) {
        mcp_tool_catalog_remove(tool_name);
    }

    /* Remove from enabled set. */
    if (err == ESP_OK) {
        size_t idx = find_enabled(tool_name);
        if (idx < s_enabled_count) {
            remove_enabled_locked(idx);
        }
    }

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

    ESP_LOGI(TAG, "Disabled tool: %s.%s", device_id, command);
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
    out->state = (mcp_exposure_state_t)rec->state;
    out->reason = (mcp_exposure_reason_t)rec->reason;
    memcpy(out->capability_digest, rec->capability_digest,
           MCP_CAPABILITY_DIGEST_LEN);

    /* Regenerate tool name deterministically. */
    out->tool_name[0] = '\0';
    generate_tool_name_for_device(device_id, command, out->tool_name,
                                  sizeof(out->tool_name));

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
        out[i].state = (mcp_exposure_state_t)rec->state;
        out[i].reason = (mcp_exposure_reason_t)rec->reason;
        memcpy(out[i].capability_digest, rec->capability_digest,
               MCP_CAPABILITY_DIGEST_LEN);
        out[i].tool_name[0] = '\0';
        generate_tool_name_for_device(rec->device_id, rec->command,
                                      out[i].tool_name,
                                      sizeof(out[i].tool_name));
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
    mcp_tool_catalog_remove_device(device_id);

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
    }

    ESP_LOGI(TAG, "Revoked all tools for device: %s", device_id);
    return ESP_OK;
}

esp_err_t mcp_tool_exposure_refresh_device_name(const char *device_id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (device_id == NULL || device_id[0] == '\0') return ESP_ERR_INVALID_ARG;

    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    device_capability_snapshot_t cap;
    if (device_capabilities_get(device_id, &cap) != ESP_OK ||
        !cap.has_committed) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Validate every replacement before changing the live catalog. */
    esp_err_t validation = ESP_OK;
    for (size_t i = 0; i < s_enabled_count && validation == ESP_OK; i++) {
        if (strcmp(s_enabled[i].device_id, device_id) != 0) continue;
        char desired[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
        if (strcmp(entry.name, entry.device_id) == 0) {
            validation = ESP_ERR_INVALID_STATE;
            break;
        }
        validation = mcp_tool_name_generate(entry.name, s_enabled[i].command,
                                            desired, sizeof(desired));
        if (validation != ESP_OK) break;
        bool command_found = false;
        for (size_t c = 0; c < cap.count; c++) {
            if (strcmp(cap.items[c].command, s_enabled[i].command) == 0) {
                command_found = true;
                break;
            }
        }
        if (!command_found) {
            validation = ESP_ERR_NOT_FOUND;
            break;
        }
        size_t collision = find_enabled(desired);
        if (collision < s_enabled_count && collision != i) {
            validation = ESP_ERR_INVALID_STATE;
        }
    }

    if (validation != ESP_OK) {
        ESP_LOGW(TAG,
                 "Device name cannot form unique MCP tools; revoking %s",
                 device_id);
        mcp_tool_catalog_remove_device(device_id);
        for (size_t i = s_enabled_count; i > 0; ) {
            i--;
            if (strcmp(s_enabled[i].device_id, device_id) == 0) {
                remove_enabled_locked(i);
            }
        }
        for (size_t i = 0; i < s_persisted_count; i++) {
            if (strcmp(s_persisted[i].device_id, device_id) == 0 &&
                s_persisted[i].state == MCP_EXPOSURE_ENABLED) {
                s_persisted[i].state = MCP_EXPOSURE_NEEDS_REVIEW;
                s_persisted[i].reason = MCP_EXPOSURE_REASON_POLICY_BLOCKED;
            }
        }
        persist_save_locked();
        xSemaphoreGive(s_mutex);
        return validation;
    }

    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < s_enabled_count; i++) {
        enabled_entry_t *enabled = &s_enabled[i];
        if (strcmp(enabled->device_id, device_id) != 0) continue;

        char desired[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
        mcp_tool_name_generate(entry.name, enabled->command,
                               desired, sizeof(desired));
        if (strcmp(desired, enabled->tool_name) == 0) continue;

        const device_capability_t *target = NULL;
        for (size_t c = 0; c < cap.count; c++) {
            if (strcmp(cap.items[c].command, enabled->command) == 0) {
                target = &cap.items[c];
                break;
            }
        }
        if (target == NULL) {
            result = ESP_ERR_NOT_FOUND;
            break;
        }

        char previous[MCP_DYNAMIC_TOOL_NAME_MAX + 1];
        strlcpy(previous, enabled->tool_name, sizeof(previous));
        mcp_tool_binding_t binding = {0};
        strlcpy(binding.tool_name, desired, sizeof(binding.tool_name));
        strlcpy(binding.device_id, enabled->device_id,
                sizeof(binding.device_id));
        strlcpy(binding.command, enabled->command, sizeof(binding.command));
        memcpy(&binding.capability, target, sizeof(binding.capability));

        mcp_tool_catalog_remove(previous);
        result = mcp_tool_catalog_add(&binding);
        if (result != ESP_OK) {
            strlcpy(binding.tool_name, previous, sizeof(binding.tool_name));
            mcp_tool_catalog_add(&binding);
            break;
        }
        strlcpy(enabled->tool_name, desired, sizeof(enabled->tool_name));
    }

    if (result == ESP_OK) {
        for (size_t i = 0; i < s_persisted_count; i++) {
            if (strcmp(s_persisted[i].device_id, device_id) == 0) {
                s_persisted[i].naming_version = MCP_EXP_NAMING_VERSION;
            }
        }
        result = persist_save_locked();
    }
    xSemaphoreGive(s_mutex);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Refreshed MCP tool names for device '%s'", entry.name);
    }
    return result;
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
