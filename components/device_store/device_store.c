#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "device_store.h"

static const char *TAG = "device_store";
static const char *NVS_NAMESPACE = "dev_list";

static device_entry_t s_cache[DEVICE_STORE_MAX_DEVICES];
static int s_count;
static SemaphoreHandle_t s_mutex;

static bool lock_store(void)
{
    return s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void unlock_store(void)
{
    xSemaphoreGive(s_mutex);
}

static device_entry_t *find_unlocked(const char *device_id)
{
    if (device_id == NULL) return NULL;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_cache[i].device_id, device_id) == 0) return &s_cache[i];
    }
    return NULL;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* NimBLE stores addresses least-significant byte first. */
static int parse_ble_addr(const char *text, uint8_t addr[6])
{
    if (text == NULL || addr == NULL) return -1;

    uint8_t display_order[6];
    for (int i = 0; i < 6; i++) {
        int high = hex_value(*text++);
        if (high < 0) return -1;
        int low = hex_value(*text++);
        if (low < 0) return -1;
        display_order[i] = (uint8_t)((high << 4) | low);
        if (i < 5) {
            if (*text != ':' && *text != '-') return -1;
            text++;
        }
    }
    if (*text != '\0') return -1;

    for (int i = 0; i < 6; i++) addr[i] = display_order[5 - i];
    return 0;
}

static esp_err_t save_metadata(nvs_handle_t handle, int count)
{
    esp_err_t err = nvs_set_u8(handle, "count", (uint8_t)count);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "schema_ver", DEVICE_STORE_SCHEMA_VERSION);
    }
    return err;
}

static esp_err_t save_entry(nvs_handle_t handle, int index, const device_entry_t *entry)
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
    } else if (parse_ble_addr(entry->device_id, entry->ble_addr) == 0) {
        /* Migration path for old records that used the MAC as device_id. */
        entry->ble_addr_type = 0;
        entry->has_ble_addr = 1;
    }

    entry->connected = 0;
    return ESP_OK;
}

static esp_err_t load_from_nvs(void)
{
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
    if (stored_count > DEVICE_STORE_MAX_DEVICES) {
        ESP_LOGW(TAG, "Stored count %u exceeds limit; truncating to %d", stored_count,
                 DEVICE_STORE_MAX_DEVICES);
        stored_count = DEVICE_STORE_MAX_DEVICES;
    }

    int loaded_count = 0;
    for (int i = 0; i < stored_count; i++) {
        device_entry_t entry;
        err = load_entry(handle, i, &entry);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Ignoring corrupt device entry %d: %s", i, esp_err_to_name(err));
            continue;
        }
        s_cache[loaded_count++] = entry;
    }
    s_count = loaded_count;

    if (stored_version != DEVICE_STORE_SCHEMA_VERSION || loaded_count != stored_count) {
        err = save_metadata(handle, loaded_count);
        for (int i = 0; err == ESP_OK && i < loaded_count; i++) {
            err = save_entry(handle, i, &s_cache[i]);
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
        ESP_LOGI(TAG, "Loaded %d devices from NVS (schema v%d)", s_count,
                 DEVICE_STORE_SCHEMA_VERSION);
    }
    return err;
}

static bool valid_text(const char *value, size_t max_length, bool allow_empty)
{
    if (value == NULL) return false;
    size_t length = strnlen(value, max_length);
    return length < max_length && (allow_empty || length > 0);
}

int device_store_init(void)
{
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL || !lock_store()) return -1;

    memset(s_cache, 0, sizeof(s_cache));
    s_count = 0;
    esp_err_t err = load_from_nvs();
    unlock_store();
    return err == ESP_OK ? 0 : -1;
}

int device_store_add(const char *device_id, const char *name, const char *type)
{
    if (!valid_text(device_id, DEVICE_ID_MAX_LEN, false) ||
        !valid_text(name, DEVICE_NAME_MAX_LEN, false) ||
        !valid_text(type, DEVICE_TYPE_MAX_LEN, false) || !lock_store()) {
        return -1;
    }

    if (s_count >= DEVICE_STORE_MAX_DEVICES || find_unlocked(device_id) != NULL) {
        unlock_store();
        return -1;
    }

    device_entry_t entry = {0};
    strlcpy(entry.device_id, device_id, sizeof(entry.device_id));
    strlcpy(entry.name, name, sizeof(entry.name));
    strlcpy(entry.type, type, sizeof(entry.type));
    if (parse_ble_addr(device_id, entry.ble_addr) == 0) {
        entry.has_ble_addr = 1;
        entry.ble_addr_type = 0;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = save_entry(handle, s_count, &entry);
    if (err == ESP_OK) err = save_metadata(handle, s_count + 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err == ESP_OK) s_cache[s_count++] = entry;
    if (handle != 0) nvs_close(handle);
    unlock_store();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist device: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "Added device: %s (%s, type=%s)", device_id, name, type);
    return 0;
}

int device_store_delete(const char *device_id)
{
    if (device_id == NULL || !lock_store()) return -1;

    int index = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_cache[i].device_id, device_id) == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        unlock_store();
        return -1;
    }

    device_entry_t compacted[DEVICE_STORE_MAX_DEVICES];
    memcpy(compacted, s_cache, sizeof(compacted));
    for (int i = index; i < s_count - 1; i++) compacted[i] = compacted[i + 1];
    memset(&compacted[s_count - 1], 0, sizeof(compacted[0]));

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    for (int i = index; err == ESP_OK && i < s_count - 1; i++) {
        err = save_entry(handle, i, &compacted[i]);
    }
    if (err == ESP_OK) err = erase_entry(handle, s_count - 1);
    if (err == ESP_OK) err = save_metadata(handle, s_count - 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err == ESP_OK) {
        memcpy(s_cache, compacted, sizeof(s_cache));
        s_count--;
    }
    if (handle != 0) nvs_close(handle);
    unlock_store();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete device: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "Deleted device: %s", device_id);
    return 0;
}

int device_store_edit(const char *device_id, const char *new_name, const char *new_type)
{
    if (device_id == NULL ||
        (new_name != NULL && !valid_text(new_name, DEVICE_NAME_MAX_LEN, false)) ||
        (new_type != NULL && !valid_text(new_type, DEVICE_TYPE_MAX_LEN, false)) ||
        (new_name == NULL && new_type == NULL) || !lock_store()) {
        return -1;
    }

    device_entry_t *entry = find_unlocked(device_id);
    if (entry == NULL) {
        unlock_store();
        return -1;
    }

    device_entry_t updated = *entry;
    if (new_name != NULL) strlcpy(updated.name, new_name, sizeof(updated.name));
    if (new_type != NULL) strlcpy(updated.type, new_type, sizeof(updated.type));
    int index = (int)(entry - s_cache);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = save_entry(handle, index, &updated);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err == ESP_OK) *entry = updated;
    if (handle != 0) nvs_close(handle);
    unlock_store();
    return err == ESP_OK ? 0 : -1;
}

const device_entry_t *device_store_list(int *out_count)
{
    /* Compatibility API. New concurrent code should use device_store_snapshot(). */
    if (out_count != NULL) *out_count = s_count;
    return s_cache;
}

device_entry_t *device_store_find(const char *device_id)
{
    /* Compatibility API. New concurrent code should use device_store_get(). */
    return find_unlocked(device_id);
}

int device_store_get(const char *device_id, device_entry_t *out_entry)
{
    if (device_id == NULL || out_entry == NULL || !lock_store()) return -1;
    device_entry_t *entry = find_unlocked(device_id);
    if (entry != NULL) *out_entry = *entry;
    unlock_store();
    return entry == NULL ? -1 : 0;
}

int device_store_snapshot(device_entry_t *out_entries, size_t max_entries)
{
    if ((out_entries == NULL && max_entries > 0) || !lock_store()) return -1;
    size_t count = (size_t)s_count < max_entries ? (size_t)s_count : max_entries;
    if (count > 0) memcpy(out_entries, s_cache, count * sizeof(s_cache[0]));
    unlock_store();
    return (int)count;
}

int device_store_set_ble_addr(const char *device_id, const uint8_t ble_addr[6],
                              uint8_t addr_type)
{
    if (device_id == NULL || ble_addr == NULL || !lock_store()) return -1;
    device_entry_t *entry = find_unlocked(device_id);
    if (entry == NULL) {
        unlock_store();
        return -1;
    }

    if (entry->has_ble_addr && entry->ble_addr_type == addr_type &&
        memcmp(entry->ble_addr, ble_addr, sizeof(entry->ble_addr)) == 0) {
        unlock_store();
        return 0;
    }

    device_entry_t updated = *entry;
    memcpy(updated.ble_addr, ble_addr, sizeof(updated.ble_addr));
    updated.ble_addr_type = addr_type;
    updated.has_ble_addr = 1;
    int index = (int)(entry - s_cache);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = save_entry(handle, index, &updated);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err == ESP_OK) *entry = updated;
    if (handle != 0) nvs_close(handle);
    unlock_store();
    return err == ESP_OK ? 0 : -1;
}

void device_store_set_connected(const char *device_id, int connected)
{
    if (device_id == NULL || !lock_store()) return;
    device_entry_t *entry = find_unlocked(device_id);
    if (entry != NULL) entry->connected = connected != 0;
    unlock_store();
}
