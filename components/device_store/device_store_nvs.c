#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "device_store_internal.h"

static const char *TAG = "device_store";
static const char *NVS_NAMESPACE = "dev_list";

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
    if (entry->has_ble_addr) {
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

static esp_err_t read_required_string(nvs_handle_t handle, const char *key,
                                      char *out, size_t out_size)
{
    size_t length = out_size;
    esp_err_t err = nvs_get_str(handle, key, out, &length);
    if (err == ESP_OK && (length == 0 || out[0] == '\0')) return ESP_ERR_INVALID_SIZE;
    return err;
}

static esp_err_t load_entry(nvs_handle_t handle, int index, device_entry_t *entry)
{
    char key[16];
    memset(entry, 0, sizeof(*entry));

    snprintf(key, sizeof(key), "id_%d", index);
    esp_err_t err = read_required_string(handle, key, entry->device_id,
                                         sizeof(entry->device_id));
    if (err != ESP_OK) return err;

    snprintf(key, sizeof(key), "name_%d", index);
    if (read_required_string(handle, key, entry->name, sizeof(entry->name)) != ESP_OK) {
        strlcpy(entry->name, entry->device_id, sizeof(entry->name));
    }

    snprintf(key, sizeof(key), "type_%d", index);
    if (read_required_string(handle, key, entry->type, sizeof(entry->type)) != ESP_OK) {
        strlcpy(entry->type, "generic", sizeof(entry->type));
    }

    snprintf(key, sizeof(key), "addr_%d", index);
    size_t address_len = sizeof(entry->ble_addr);
    if (nvs_get_blob(handle, key, entry->ble_addr, &address_len) == ESP_OK &&
        address_len == sizeof(entry->ble_addr)) {
        snprintf(key, sizeof(key), "atype_%d", index);
        if (nvs_get_u8(handle, key, &entry->ble_addr_type) != ESP_OK) {
            entry->ble_addr_type = 0;
        }
        entry->has_ble_addr = 1;
    } else if (device_store_entry_parse_ble_addr(entry->device_id,
                                                  entry->ble_addr) == 0) {
        /* Migration path for old records that used the MAC as device_id. */
        entry->ble_addr_type = 0;
        entry->has_ble_addr = 1;
    }

    entry->connected = 0;
    return ESP_OK;
}

esp_err_t device_store_nvs_load(device_entry_t *entries, size_t capacity,
                                int *out_count)
{
    if (entries == NULL || out_count == NULL || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t stored_version = 0;
    esp_err_t version_err = nvs_get_u8(handle, "schema_ver", &stored_version);
    if (version_err == ESP_ERR_NVS_NOT_FOUND) stored_version = 1;
    if (stored_version != DEVICE_STORE_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "Migrating device schema v%u to v%d", stored_version,
                 DEVICE_STORE_SCHEMA_VERSION);
    }

    uint8_t stored_count = 0;
    if (nvs_get_u8(handle, "count", &stored_count) != ESP_OK) stored_count = 0;
    if ((size_t)stored_count > capacity) {
        ESP_LOGW(TAG, "Stored count %u exceeds limit; truncating to %u", stored_count,
                 (unsigned)capacity);
        stored_count = (uint8_t)capacity;
    }

    int loaded_count = 0;
    for (int i = 0; i < stored_count; i++) {
        device_entry_t entry;
        err = load_entry(handle, i, &entry);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Ignoring corrupt device entry %d: %s", i,
                     esp_err_to_name(err));
            continue;
        }
        entries[loaded_count++] = entry;
    }
    *out_count = loaded_count;

    if (stored_version != DEVICE_STORE_SCHEMA_VERSION || loaded_count != stored_count) {
        err = save_metadata(handle, loaded_count);
        for (int i = 0; err == ESP_OK && i < loaded_count; i++) {
            err = save_entry(handle, i, &entries[i]);
        }
        for (int i = loaded_count; err == ESP_OK && i < stored_count; i++) {
            err = erase_entry(handle, i);
        }
        if (err == ESP_OK) err = nvs_commit(handle);
    } else {
        err = ESP_OK;
    }

    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded %d devices from NVS (schema v%d)", loaded_count,
                 DEVICE_STORE_SCHEMA_VERSION);
    }
    return err;
}

esp_err_t device_store_nvs_append(int index, int new_count,
                                  const device_entry_t *entry)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = save_entry(handle, index, entry);
    if (err == ESP_OK) err = save_metadata(handle, new_count);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return err;
}

esp_err_t device_store_nvs_update(int index, const device_entry_t *entry)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = save_entry(handle, index, entry);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return err;
}

esp_err_t device_store_nvs_delete(int index,
                                  const device_entry_t *compacted_entries,
                                  int previous_count)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    for (int i = index; err == ESP_OK && i < previous_count - 1; i++) {
        err = save_entry(handle, i, &compacted_entries[i]);
    }
    if (err == ESP_OK) err = erase_entry(handle, previous_count - 1);
    if (err == ESP_OK) err = save_metadata(handle, previous_count - 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return err;
}
