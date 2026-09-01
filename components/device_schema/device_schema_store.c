#include "device_schema_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "device_store.h"
#include "esp_log.h"
#include "nvs.h"

#define SCHEMA_STORE_SCHEMA_VERSION 1
#define SCHEMA_NVS_NAMESPACE "dev_schema"

static const char *TAG = "schema_store";

/* ── NVS persisted blob ─────────────────────────────────────────────── */

typedef struct {
    uint8_t schema_version;
    uint8_t tool_count;
    uint8_t feature_count;
    uint16_t reserved;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t revision;
    device_schema_tool_t tools[DEVICE_SCHEMA_MAX_TOOLS];
    device_schema_feature_t features[DEVICE_SCHEMA_MAX_FEATURES];
} persisted_schema_t;

/* ── Key generation ─────────────────────────────────────────────────── */

static void schema_nvs_key(int index, char key[8])
{
    unsigned bounded = (unsigned)index % DEVICE_STORE_MAX_DEVICES;
    snprintf(key, 8, "sch%02u", bounded);
}

/* ── Persist ────────────────────────────────────────────────────────── */

esp_err_t schema_persist_record(int index,
                                const device_schema_snapshot_t *snapshot)
{
    persisted_schema_t persisted = {
        .schema_version = SCHEMA_STORE_SCHEMA_VERSION,
        .tool_count = (uint8_t)snapshot->tool_count,
        .feature_count = (uint8_t)snapshot->feature_count,
        .revision = snapshot->revision,
    };
    strlcpy(persisted.device_id, snapshot->device_id,
            sizeof(persisted.device_id));
    if (snapshot->tool_count > 0) {
        memcpy(persisted.tools, snapshot->tools,
               snapshot->tool_count * sizeof(snapshot->tools[0]));
    }
    if (snapshot->feature_count > 0) {
        memcpy(persisted.features, snapshot->features,
               snapshot->feature_count * sizeof(snapshot->features[0]));
    }

    nvs_handle_t handle;
    esp_err_t error = nvs_open(SCHEMA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    char key[8];
    schema_nvs_key(index, key);
    /* Always write the full struct so the on-disk layout matches the struct
       layout.  A compact blob (header + N tools + M features) would misalign
       when tool_count < MAX_TOOLS because features[] sits at a fixed offset
       past MAX_TOOLS slots in the struct. */
    error = nvs_set_blob(handle, key, &persisted, sizeof(persisted));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

/* ── Load ───────────────────────────────────────────────────────────── */

void schema_load_persisted(schema_record_t *records)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(SCHEMA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not open schema NVS: %s",
                 esp_err_to_name(error));
        return;
    }

    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        char key[8];
        schema_nvs_key(i, key);
        persisted_schema_t persisted;
        memset(&persisted, 0, sizeof(persisted));
        size_t length = 0;
        error = nvs_get_blob(handle, key, NULL, &length);
        if (error == ESP_ERR_NVS_NOT_FOUND) continue;
        if (error != ESP_OK ||
            length < offsetof(persisted_schema_t, tools) ||
            length > sizeof(persisted)) {
            ESP_LOGW(TAG, "Ignoring invalid schema record %s", key);
            continue;
        }
        error = nvs_get_blob(handle, key, &persisted, &length);
        /* Reject compact blobs — their features would land in the wrong
           struct offset.  They are re-persisted as full-struct on next
           discovery cycle. */
        if (error != ESP_OK ||
            length != sizeof(persisted_schema_t) ||
            persisted.schema_version != SCHEMA_STORE_SCHEMA_VERSION ||
            persisted.tool_count > DEVICE_SCHEMA_MAX_TOOLS ||
            persisted.feature_count > DEVICE_SCHEMA_MAX_FEATURES ||
            persisted.device_id[0] == '\0') {
            ESP_LOGW(TAG, "Ignoring invalid schema record %s", key);
            continue;
        }

        device_entry_t device;
        if (device_store_get(persisted.device_id, &device) != DEVICE_STORE_OK) {
            continue;
        }

        bool valid = true;
        for (size_t t = 0; t < persisted.tool_count; t++) {
            if (!schema_valid_tool(&persisted.tools[t])) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;

        schema_record_t *record = &records[i];
        memset(record, 0, sizeof(*record));
        record->used = true;
        record->has_committed = true;
        record->persist_dirty = false;
        strlcpy(record->committed.device_id, persisted.device_id,
                sizeof(record->committed.device_id));
        record->committed.state = DEVICE_SCHEMA_STATE_READY;
        record->committed.revision = persisted.revision;
        record->committed.tool_count = persisted.tool_count;
        record->committed.feature_count = persisted.feature_count;
        memcpy(record->committed.tools, persisted.tools,
               persisted.tool_count * sizeof(persisted.tools[0]));
        memcpy(record->committed.features, persisted.features,
               persisted.feature_count * sizeof(persisted.features[0]));

        ESP_LOGI(TAG, "[%s] loaded cached schema (revision=%lu, %u tools, %u features)",
                 persisted.device_id, (unsigned long)persisted.revision,
                 (unsigned)persisted.tool_count,
                 (unsigned)persisted.feature_count);
    }
    nvs_close(handle);
}

/* ── Erase ──────────────────────────────────────────────────────────── */

esp_err_t schema_erase_nvs(int index)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(SCHEMA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    char key[8];
    schema_nvs_key(index, key);
    error = nvs_erase_key(handle, key);
    if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

/* ── Legacy dev_caps cleanup ────────────────────────────────────────── */
/* V4-04: Erase stale capability-v3 NVS entries.  Non-blocking: if the
   old namespace doesn't exist or cleanup fails, log and continue. */

#define LEGACY_CAPS_NAMESPACE "dev_caps"

void schema_cleanup_legacy_caps(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(LEGACY_CAPS_NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return;
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not open legacy caps namespace: %s",
                 esp_err_to_name(error));
        return;
    }
    bool any_erased = false;
    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        char key[8];
        snprintf(key, sizeof(key), "cap%02u", (unsigned)i);
        esp_err_t err = nvs_erase_key(handle, key);
        if (err == ESP_OK) {
            any_erased = true;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Could not erase legacy key %s: %s",
                     key, esp_err_to_name(err));
        }
    }
    if (any_erased) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "Cleaned up legacy dev_caps namespace");
    }
    nvs_close(handle);
}
