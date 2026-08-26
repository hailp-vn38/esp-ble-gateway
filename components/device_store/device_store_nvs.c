#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "device_store_internal.h"

static const char *TAG = "device_store";
static const char *NVS_NAMESPACE = "dev_list";

static device_store_result_t result_from(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return DEVICE_STORE_OK;
    case ESP_ERR_NVS_TYPE_MISMATCH:
    case ESP_ERR_NVS_INVALID_LENGTH:
        return DEVICE_STORE_ERR_CORRUPT;
    default:
        return DEVICE_STORE_ERR_PERSISTENCE;
    }
}

static esp_err_t save_metadata(nvs_handle_t handle, int count)
{
    esp_err_t err = nvs_set_u8(handle, "count", (uint8_t)count);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "schema_ver", DEVICE_STORE_SCHEMA_VERSION);
    }
    return err;
}

static esp_err_t save_entry(nvs_handle_t handle, int index,
                            const device_entry_t *entry)
{
    char key[16];

    snprintf(key, sizeof(key), "id_%d", index);
    esp_err_t err = nvs_set_str(handle, key, entry->device_id);
    if (err != ESP_OK) return err;

    snprintf(key, sizeof(key), "name_%d", index);
    err = nvs_set_str(handle, key, entry->name);
    if (err != ESP_OK) return err;

    snprintf(key, sizeof(key), "type_%d", index);
    err = nvs_set_str(handle, key, entry->type);
    if (err != ESP_OK) return err;

    snprintf(key, sizeof(key), "addr_%d", index);
    if (entry->has_ble_identity) {
        err = nvs_set_blob(handle, key, entry->ble_addr, sizeof(entry->ble_addr));
        if (err != ESP_OK) return err;

        snprintf(key, sizeof(key), "atype_%d", index);
        return nvs_set_u8(handle, key, entry->ble_addr_type);
    }

    esp_err_t erase_err = nvs_erase_key(handle, key);
    if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) return erase_err;
    snprintf(key, sizeof(key), "atype_%d", index);
    erase_err = nvs_erase_key(handle, key);
    return erase_err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : erase_err;
}

static esp_err_t erase_entry(nvs_handle_t handle, int index)
{
    static const char *const prefixes[] = {"id", "name", "type", "addr", "atype"};
    char key[16];
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        snprintf(key, sizeof(key), "%s_%d", prefixes[i], index);
        esp_err_t err = nvs_erase_key(handle, key);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    }
    return ESP_OK;
}

device_store_result_t device_store_nvs_load(device_entry_t *entries,
                                            size_t capacity, size_t *out_count)
{
    if (entries == NULL || out_count == NULL || capacity == 0) {
        return DEVICE_STORE_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return DEVICE_STORE_ERR_PERSISTENCE;
    }

    // Stored schema governs everything below. A store written by newer
    // firmware must never be rewritten or downgraded (refactor plan §9).
    uint8_t stored_version = 0;
    err = nvs_get_u8(handle, "schema_ver", &stored_version);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        stored_version = 1; // Pre-schema stores behave as v1.
    } else if (err != ESP_OK) {
        nvs_close(handle);
        return result_from(err);
    }

    if (stored_version > DEVICE_STORE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Device store schema v%u is newer than supported v%d; "
                      "NVS left untouched",
                 stored_version, DEVICE_STORE_SCHEMA_VERSION);
        nvs_close(handle);
        return DEVICE_STORE_ERR_SCHEMA_TOO_NEW;
    }

    uint8_t raw_count = 0;
    err = nvs_get_u8(handle, "count", &raw_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        raw_count = 0; // Fresh store.
    } else if (err != ESP_OK) {
        nvs_close(handle);
        return result_from(err);
    }

    // Never clamp or repair a store that exceeds this firmware's capacity:
    // rewriting would orphan records irreversibly (refactor plan §18).
    if ((size_t)raw_count > capacity) {
        ESP_LOGE(TAG, "Stored device count %u exceeds firmware capacity %u",
                 raw_count, (unsigned)capacity);
        nvs_close(handle);
        return DEVICE_STORE_ERR_CAPACITY_EXCEEDED;
    }

    size_t loaded = 0;
    device_store_result_t result = DEVICE_STORE_OK;
    for (int i = 0; i < raw_count; i++) {
        device_entry_t entry;
        result = device_store_migration_load_entry(handle, stored_version, i,
                                                   &entry);
        if (result == DEVICE_STORE_ERR_CORRUPT) {
            ESP_LOGW(TAG, "Ignoring corrupt device record %d (schema v%u)", i,
                     stored_version);
            continue;
        }
        if (result != DEVICE_STORE_OK) break;
        entries[loaded++] = entry;
    }
    if (result != DEVICE_STORE_OK && result != DEVICE_STORE_ERR_CORRUPT) {
        nvs_close(handle);
        return result;
    }

    // Rewrite only to migrate an older schema or compact away skipped
    // corrupt records — never to force current capacity onto data.
    if (stored_version != DEVICE_STORE_SCHEMA_VERSION ||
        loaded != (size_t)raw_count) {
        ESP_LOGW(TAG, "Migrating device store schema v%u -> v%d (%u records)",
                 stored_version, DEVICE_STORE_SCHEMA_VERSION, (unsigned)loaded);
        err = save_metadata(handle, (int)loaded);
        for (size_t i = 0; err == ESP_OK && i < loaded; i++) {
            err = save_entry(handle, (int)i, &entries[i]);
        }
        for (int i = (int)loaded; err == ESP_OK && i < raw_count; i++) {
            err = erase_entry(handle, i);
        }
        if (err == ESP_OK) err = nvs_commit(handle);
        if (err != ESP_OK) {
            nvs_close(handle);
            return result_from(err);
        }
    }

    nvs_close(handle);
    *out_count = loaded;
    ESP_LOGI(TAG, "Loaded %u devices from NVS (schema v%d)", (unsigned)loaded,
             DEVICE_STORE_SCHEMA_VERSION);
    return DEVICE_STORE_OK;
}

device_store_result_t device_store_nvs_append(size_t index, size_t new_count,
                                              const device_entry_t *entry)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = save_entry(handle, (int)index, entry);
    if (err == ESP_OK) err = save_metadata(handle, (int)new_count);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return result_from(err);
}

device_store_result_t device_store_nvs_update(size_t index,
                                              const device_entry_t *entry)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = save_entry(handle, (int)index, entry);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return result_from(err);
}

device_store_result_t device_store_nvs_delete(size_t index,
                                              const device_entry_t *compacted,
                                              size_t previous_count)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    for (size_t i = index; err == ESP_OK && i < previous_count - 1; i++) {
        err = save_entry(handle, (int)i, &compacted[i]);
    }
    if (err == ESP_OK) err = erase_entry(handle, (int)(previous_count - 1));
    if (err == ESP_OK) err = save_metadata(handle, (int)(previous_count - 1));
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return result_from(err);
}
