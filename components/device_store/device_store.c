#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "device_store.h"
#include "device_store_internal.h"

static const char *TAG = "device_store";

static device_entry_t s_cache[DEVICE_STORE_MAX_DEVICES];
static size_t s_count;
static SemaphoreHandle_t s_mutex;
static bool s_initialized;

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
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_cache[i].device_id, device_id) == 0) return &s_cache[i];
    }
    return NULL;
}

// Another device already owns this canonical transport identity?
static bool identity_in_use_unlocked(const char *device_id,
                                     const uint8_t ble_addr[6],
                                     uint8_t addr_type)
{
    for (size_t i = 0; i < s_count; i++) {
        const device_entry_t *entry = &s_cache[i];
        if (!entry->has_ble_identity || strcmp(entry->device_id, device_id) == 0) {
            continue;
        }
        if (entry->ble_addr_type == addr_type &&
            memcmp(entry->ble_addr, ble_addr, sizeof(entry->ble_addr)) == 0) {
            return true;
        }
    }
    return false;
}

device_store_result_t device_store_init(void)
{
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL || !lock_store()) return DEVICE_STORE_ERR_INVALID_STATE;

    if (s_initialized) {
        unlock_store();
        return DEVICE_STORE_ERR_INVALID_STATE;
    }

    memset(s_cache, 0, sizeof(s_cache));
    s_count = 0;
    size_t loaded = 0;
    device_store_result_t result =
        device_store_nvs_load(s_cache, DEVICE_STORE_MAX_DEVICES, &loaded);
    if (result == DEVICE_STORE_OK) {
        s_count = loaded;
        s_initialized = true;
    }
    unlock_store();
    return result;
}

device_store_result_t device_store_add(const char *device_id, const char *name)
{
    device_entry_t entry;
    if (!device_store_entry_create(&entry, device_id, name)) {
        return DEVICE_STORE_ERR_INVALID_ARG;
    }
    if (!lock_store()) return DEVICE_STORE_ERR_BUSY;
    if (!s_initialized) {
        unlock_store();
        return DEVICE_STORE_ERR_INVALID_STATE;
    }

    if (find_unlocked(device_id) != NULL) {
        unlock_store();
        return DEVICE_STORE_ERR_DUPLICATE_ID;
    }
    if (s_count >= DEVICE_STORE_MAX_DEVICES) {
        unlock_store();
        return DEVICE_STORE_ERR_FULL;
    }

    size_t index = s_count;
    device_store_result_t result =
        device_store_nvs_append(index, s_count + 1, &entry);
    if (result == DEVICE_STORE_OK) {
        s_cache[s_count++] = entry;
    }
    unlock_store();

    if (result != DEVICE_STORE_OK) {
        ESP_LOGE(TAG, "Failed to persist device %s: %d", device_id, result);
    }
    return result;
}

device_store_result_t device_store_delete(const char *device_id)
{
    if (device_id == NULL) return DEVICE_STORE_ERR_INVALID_ARG;
    if (!lock_store()) return DEVICE_STORE_ERR_BUSY;
    if (!s_initialized) {
        unlock_store();
        return DEVICE_STORE_ERR_INVALID_STATE;
    }

    int index = -1;
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_cache[i].device_id, device_id) == 0) {
            index = (int)i;
            break;
        }
    }
    if (index < 0) {
        unlock_store();
        return DEVICE_STORE_ERR_NOT_FOUND;
    }

    device_entry_t compacted[DEVICE_STORE_MAX_DEVICES];
    memcpy(compacted, s_cache, sizeof(compacted));
    for (int i = index; i < (int)s_count - 1; i++) compacted[i] = compacted[i + 1];
    memset(&compacted[s_count - 1], 0, sizeof(compacted[0]));

    size_t previous_count = s_count;
    device_store_result_t result = device_store_nvs_delete(
        (size_t)index, compacted, previous_count);
    if (result == DEVICE_STORE_OK) {
        memcpy(s_cache, compacted, sizeof(s_cache));
        s_count--;
    }
    unlock_store();

    if (result != DEVICE_STORE_OK) {
        ESP_LOGE(TAG, "Failed to delete device %s: %d", device_id, result);
    }
    return result;
}

device_store_result_t device_store_edit(const char *device_id,
                                        const char *new_name)
{
    if (device_id == NULL) return DEVICE_STORE_ERR_INVALID_ARG;
    if (!lock_store()) return DEVICE_STORE_ERR_BUSY;
    if (!s_initialized) {
        unlock_store();
        return DEVICE_STORE_ERR_INVALID_STATE;
    }

    device_entry_t *entry = find_unlocked(device_id);
    if (entry == NULL) {
        unlock_store();
        return DEVICE_STORE_ERR_NOT_FOUND;
    }

    device_entry_t updated = *entry;
    if (!device_store_entry_edit(&updated, new_name)) {
        unlock_store();
        return DEVICE_STORE_ERR_INVALID_ARG;
    }

    size_t index = (size_t)(entry - s_cache);
    device_store_result_t result = device_store_nvs_update(index, &updated);
    if (result == DEVICE_STORE_OK) *entry = updated;
    unlock_store();
    return result;
}

device_store_result_t device_store_get(const char *device_id,
                                       device_entry_t *out_entry)
{
    if (device_id == NULL || out_entry == NULL) return DEVICE_STORE_ERR_INVALID_ARG;
    if (!lock_store()) return DEVICE_STORE_ERR_BUSY;
    if (!s_initialized) {
        unlock_store();
        return DEVICE_STORE_ERR_INVALID_STATE;
    }

    device_entry_t *entry = find_unlocked(device_id);
    if (entry != NULL) *out_entry = *entry;
    unlock_store();
    return entry != NULL ? DEVICE_STORE_OK : DEVICE_STORE_ERR_NOT_FOUND;
}

device_store_result_t device_store_snapshot(device_entry_t *out_entries,
                                            size_t capacity,
                                            size_t *out_count)
{
    if (out_count == NULL) return DEVICE_STORE_ERR_INVALID_ARG;
    if (out_entries == NULL && capacity > 0) return DEVICE_STORE_ERR_INVALID_ARG;
    if (!lock_store()) return DEVICE_STORE_ERR_BUSY;
    if (!s_initialized) {
        unlock_store();
        return DEVICE_STORE_ERR_INVALID_STATE;
    }

    if (out_entries == NULL && capacity == 0) {
        // Query mode: report the required size without copying.
        *out_count = s_count;
        unlock_store();
        return DEVICE_STORE_OK;
    }

    if (capacity < s_count) {
        // Never hand out a partial list that callers could mistake for a
        // complete one; report the required size instead.
        *out_count = s_count;
        unlock_store();
        return DEVICE_STORE_ERR_BUFFER_TOO_SMALL;
    }

    if (s_count > 0) memcpy(out_entries, s_cache, s_count * sizeof(s_cache[0]));
    *out_count = s_count;
    unlock_store();
    return DEVICE_STORE_OK;
}

device_store_result_t device_store_set_ble_identity(
    const char *device_id, const uint8_t ble_addr[6], uint8_t addr_type)
{
    if (device_id == NULL || ble_addr == NULL ||
        addr_type > DEVICE_STORE_BLE_ADDR_TYPE_MAX) {
        return DEVICE_STORE_ERR_INVALID_ARG;
    }
    if (!lock_store()) return DEVICE_STORE_ERR_BUSY;
    if (!s_initialized) {
        unlock_store();
        return DEVICE_STORE_ERR_INVALID_STATE;
    }

    device_entry_t *entry = find_unlocked(device_id);
    if (entry == NULL) {
        unlock_store();
        return DEVICE_STORE_ERR_NOT_FOUND;
    }

    if (entry->has_ble_identity && entry->ble_addr_type == addr_type &&
        memcmp(entry->ble_addr, ble_addr, sizeof(entry->ble_addr)) == 0) {
        unlock_store();
        return DEVICE_STORE_OK;
    }

    if (identity_in_use_unlocked(device_id, ble_addr, addr_type)) {
        unlock_store();
        return DEVICE_STORE_ERR_DUPLICATE_BLE_IDENTITY;
    }

    device_entry_t updated = *entry;
    memcpy(updated.ble_addr, ble_addr, sizeof(updated.ble_addr));
    updated.ble_addr_type = addr_type;
    updated.has_ble_identity = true;
    size_t index = (size_t)(entry - s_cache);

    device_store_result_t result = device_store_nvs_update(index, &updated);
    if (result == DEVICE_STORE_OK) *entry = updated;
    unlock_store();
    return result;
}

void device_store_reset_for_test(void)
{
    if (!lock_store()) return;
    s_initialized = false;
    unlock_store();
}
