#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "device_store.h"
#include "device_store_internal.h"

static const char *TAG = "device_store";

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

int device_store_init(void)
{
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL || !lock_store()) return -1;

    memset(s_cache, 0, sizeof(s_cache));
    s_count = 0;
    esp_err_t err = device_store_nvs_load(
        s_cache, DEVICE_STORE_MAX_DEVICES, &s_count);
    unlock_store();
    return err == ESP_OK ? 0 : -1;
}

int device_store_add(const char *device_id, const char *name, const char *type)
{
    device_entry_t entry;
    if (!device_store_entry_create(&entry, device_id, name, type) || !lock_store()) {
        return -1;
    }

    if (s_count >= DEVICE_STORE_MAX_DEVICES || find_unlocked(device_id) != NULL) {
        unlock_store();
        return -1;
    }

    esp_err_t err = device_store_nvs_append(s_count, s_count + 1, &entry);
    if (err == ESP_OK) s_cache[s_count++] = entry;
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

    esp_err_t err = device_store_nvs_delete(index, compacted, s_count);
    if (err == ESP_OK) {
        memcpy(s_cache, compacted, sizeof(s_cache));
        s_count--;
    }
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
    if (device_id == NULL || !lock_store()) return -1;

    device_entry_t *entry = find_unlocked(device_id);
    if (entry == NULL) {
        unlock_store();
        return -1;
    }

    device_entry_t updated = *entry;
    if (!device_store_entry_edit(&updated, new_name, new_type)) {
        unlock_store();
        return -1;
    }

    int index = (int)(entry - s_cache);
    esp_err_t err = device_store_nvs_update(index, &updated);
    if (err == ESP_OK) *entry = updated;
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

    esp_err_t err = device_store_nvs_update(index, &updated);
    if (err == ESP_OK) *entry = updated;
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
