#include "device_schema_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "device_store.h"
#include "esp_log.h"
#include "memory_policy.h"
#include "nvs.h"

#define SCHEMA_STORE_SCHEMA_VERSION 3
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
} persisted_schema_v2_t;

typedef struct {
    uint8_t schema_version;
    uint8_t tool_count;
    uint8_t feature_count;
    uint16_t reserved;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint32_t revision;
    uint8_t settings_state;
    uint16_t settings_schema_revision;
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
    persisted_schema_t *persisted = gw_mem_calloc(
        1, sizeof(*persisted), GW_MEM_EXTERNAL_PREFERRED);
    if (persisted == NULL) return ESP_ERR_NO_MEM;

    persisted->schema_version = SCHEMA_STORE_SCHEMA_VERSION;
    persisted->tool_count = (uint8_t)snapshot->tool_count;
    persisted->feature_count = (uint8_t)snapshot->feature_count;
    persisted->revision = snapshot->revision;
    persisted->settings_state = (uint8_t)snapshot->settings_state;
    persisted->settings_schema_revision = snapshot->settings_schema_revision;
    strlcpy(persisted->device_id, snapshot->device_id,
            sizeof(persisted->device_id));
    if (snapshot->tool_count > 0) {
        memcpy(persisted->tools, snapshot->tools,
               snapshot->tool_count * sizeof(snapshot->tools[0]));
    }
    if (snapshot->feature_count > 0) {
        memcpy(persisted->features, snapshot->features,
               snapshot->feature_count * sizeof(snapshot->features[0]));
    }

    nvs_handle_t handle;
    esp_err_t error = nvs_open(SCHEMA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        gw_mem_free(persisted);
        return error;
    }
    char key[8];
    schema_nvs_key(index, key);
    /* Always write the full struct so the on-disk layout matches the struct
       layout.  A compact blob (header + N tools + M features) would misalign
       when tool_count < MAX_TOOLS because features[] sits at a fixed offset
       past MAX_TOOLS slots in the struct. */
    error = nvs_set_blob(handle, key, persisted, sizeof(*persisted));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    gw_mem_free(persisted);
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

    persisted_schema_t *persisted = gw_mem_calloc(
        1, sizeof(*persisted), GW_MEM_EXTERNAL_PREFERRED);
    if (persisted == NULL) {
        ESP_LOGE(TAG, "Could not allocate persisted schema load buffer");
        nvs_close(handle);
        return;
    }

    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        char key[8];
        schema_nvs_key(i, key);
        size_t length = 0;
        error = nvs_get_blob(handle, key, NULL, &length);
        if (error == ESP_ERR_NVS_NOT_FOUND) continue;
        if (error != ESP_OK || length == 0 || length > sizeof(*persisted)) {
            ESP_LOGW(TAG, "Ignoring invalid schema record %s", key);
            continue;
        }
        uint8_t *raw = gw_mem_calloc(1, length, GW_MEM_EXTERNAL_PREFERRED);
        if (raw == NULL || nvs_get_blob(handle, key, raw, &length) != ESP_OK) {
            gw_mem_free(raw);
            ESP_LOGW(TAG, "Ignoring invalid schema record %s", key);
            continue;
        }
        uint8_t stored_version = raw[0];
        if (stored_version == 2 && length == sizeof(persisted_schema_v2_t)) {
            persisted_schema_v2_t legacy;
            memcpy(&legacy, raw, sizeof(legacy));
            memset(persisted, 0, sizeof(*persisted));
            persisted->schema_version = SCHEMA_STORE_SCHEMA_VERSION;
            persisted->tool_count = legacy.tool_count;
            persisted->feature_count = legacy.feature_count;
            strlcpy(persisted->device_id, legacy.device_id,
                    sizeof(persisted->device_id));
            persisted->revision = legacy.revision;
            persisted->settings_state = DEVICE_SETTINGS_STATE_UNSUPPORTED;
            memcpy(persisted->tools, legacy.tools, sizeof(legacy.tools));
            memcpy(persisted->features, legacy.features, sizeof(legacy.features));
            ESP_LOGI(TAG, "Migrating legacy schema record %s to version 3", key);
        } else if (stored_version == SCHEMA_STORE_SCHEMA_VERSION &&
                   length == sizeof(persisted_schema_t)) {
            memcpy(persisted, raw, sizeof(*persisted));
        } else {
            ESP_LOGW(TAG, "Erasing incompatible schema record %s (version=%u)",
                     key, (unsigned)stored_version);
            nvs_erase_key(handle, key);
            nvs_commit(handle);
            gw_mem_free(raw);
            continue;
        }
        gw_mem_free(raw);
        if (persisted->tool_count > DEVICE_SCHEMA_MAX_TOOLS ||
            persisted->feature_count > DEVICE_SCHEMA_MAX_FEATURES ||
            persisted->device_id[0] == '\0') {
            ESP_LOGW(TAG, "Erasing invalid schema record %s", key);
            nvs_erase_key(handle, key);
            nvs_commit(handle);
            continue;
        }

        device_entry_t device;
        if (device_store_get(persisted->device_id, &device) != DEVICE_STORE_OK) {
            continue;
        }

        bool valid = true;
        for (size_t t = 0; t < persisted->tool_count; t++) {
            if (!schema_valid_tool(&persisted->tools[t])) {
                valid = false;
                break;
            }
        }
        for (size_t f = 0; valid && f < persisted->feature_count; f++) {
            valid = schema_valid_feature_id(persisted->features[f].feature_id) &&
                    schema_feature_matches_template(&persisted->features[f]);
        }
        if (!valid) continue;

        schema_record_t *record = &records[i];
        memset(record, 0, sizeof(*record));
        record->used = true;
        record->has_committed = true;
        record->persist_dirty = false;
        strlcpy(record->committed.device_id, persisted->device_id,
                sizeof(record->committed.device_id));
        record->committed.state = DEVICE_SCHEMA_STATE_READY;
        record->committed.revision = persisted->revision;
        record->committed.settings_state =
            persisted->settings_state <= DEVICE_SETTINGS_STATE_READY
                ? (device_settings_state_t)persisted->settings_state
                : DEVICE_SETTINGS_STATE_UNSUPPORTED;
        record->committed.settings_schema_revision =
            persisted->settings_schema_revision;
        record->committed.tool_count = persisted->tool_count;
        record->committed.feature_count = persisted->feature_count;
        memcpy(record->committed.tools, persisted->tools,
               persisted->tool_count * sizeof(persisted->tools[0]));
        memcpy(record->committed.features, persisted->features,
               persisted->feature_count * sizeof(persisted->features[0]));

        ESP_LOGI(TAG, "[%s] loaded cached schema (revision=%lu, %u tools, %u features)",
                 persisted->device_id, (unsigned long)persisted->revision,
                 (unsigned)persisted->tool_count,
                 (unsigned)persisted->feature_count);
    }
    gw_mem_free(persisted);
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
