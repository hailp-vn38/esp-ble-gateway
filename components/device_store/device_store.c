#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "device_store.h"

static const char *TAG = "device_store";
static const char *NVS_NAMESPACE = "dev_list";

static device_entry_t s_cache[DEVICE_STORE_MAX_DEVICES];
static int s_count = 0;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_ble_addr(const char *text, uint8_t addr[6])
{
    uint8_t display_order[6];
    int byte_index = 0;

    while (*text != '\0' && byte_index < 6) {
        int high = hex_value(*text++);
        if (high < 0 || *text == '\0') return -1;
        int low = hex_value(*text++);
        if (low < 0) return -1;
        display_order[byte_index++] = (uint8_t)((high << 4) | low);

        if (byte_index < 6 && (*text == ':' || *text == '-')) text++;
    }

    if (byte_index != 6 || *text != '\0') return -1;
    for (int i = 0; i < 6; i++) addr[i] = display_order[5 - i];
    return 0;
}

static esp_err_t load_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No existing device list in NVS, starting empty");
        s_count = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t count = 0;
    err = nvs_get_u8(handle, "count", &count);
    if (err != ESP_OK) count = 0;
    if (count > DEVICE_STORE_MAX_DEVICES) count = DEVICE_STORE_MAX_DEVICES;

    for (int i = 0; i < count; i++) {
        char key[16];
        size_t len;
        memset(&s_cache[i], 0, sizeof(device_entry_t));

        snprintf(key, sizeof(key), "id_%d", i);
        len = sizeof(s_cache[i].device_id);
        nvs_get_str(handle, key, s_cache[i].device_id, &len);

        snprintf(key, sizeof(key), "name_%d", i);
        len = sizeof(s_cache[i].name);
        nvs_get_str(handle, key, s_cache[i].name, &len);

        snprintf(key, sizeof(key), "type_%d", i);
        len = sizeof(s_cache[i].type);
        nvs_get_str(handle, key, s_cache[i].type, &len);

        snprintf(key, sizeof(key), "addr_%d", i);
        len = sizeof(s_cache[i].ble_addr);
        if (nvs_get_blob(handle, key, s_cache[i].ble_addr, &len) == ESP_OK &&
            len == sizeof(s_cache[i].ble_addr)) {
            snprintf(key, sizeof(key), "atype_%d", i);
            if (nvs_get_u8(handle, key, &s_cache[i].ble_addr_type) != ESP_OK) {
                s_cache[i].ble_addr_type = 0;
            }
            s_cache[i].has_ble_addr = 1;
        } else if (parse_ble_addr(s_cache[i].device_id, s_cache[i].ble_addr) == 0) {
            s_cache[i].ble_addr_type = 0;
            s_cache[i].has_ble_addr = 1;
        }

        s_cache[i].connected = 0;
    }

    s_count = count;
    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded %d devices from NVS", s_count);
    return ESP_OK;
}

static esp_err_t save_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open (write) failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(handle, "count", (uint8_t)s_count);

    for (int i = 0; i < s_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "id_%d", i);
        nvs_set_str(handle, key, s_cache[i].device_id);
        snprintf(key, sizeof(key), "name_%d", i);
        nvs_set_str(handle, key, s_cache[i].name);
        snprintf(key, sizeof(key), "type_%d", i);
        nvs_set_str(handle, key, s_cache[i].type);
        if (s_cache[i].has_ble_addr) {
            snprintf(key, sizeof(key), "addr_%d", i);
            nvs_set_blob(handle, key, s_cache[i].ble_addr, sizeof(s_cache[i].ble_addr));
            snprintf(key, sizeof(key), "atype_%d", i);
            nvs_set_u8(handle, key, s_cache[i].ble_addr_type);
        }
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    return err;
}

int device_store_init(void)
{
    memset(s_cache, 0, sizeof(s_cache));
    s_count = 0;
    return (load_from_nvs() == ESP_OK) ? 0 : -1;
}

int device_store_add(const char *device_id, const char *name, const char *type)
{
    if (device_id == NULL || name == NULL || type == NULL) return -1;
    if (s_count >= DEVICE_STORE_MAX_DEVICES) {
        ESP_LOGW(TAG, "Device store full (max=%d)", DEVICE_STORE_MAX_DEVICES);
        return -1;
    }
    if (device_store_find(device_id) != NULL) {
        ESP_LOGW(TAG, "device_id already exists: %s", device_id);
        return -1;
    }

    device_entry_t *entry = &s_cache[s_count];
    memset(entry, 0, sizeof(device_entry_t));
    strncpy(entry->device_id, device_id, sizeof(entry->device_id) - 1);
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    strncpy(entry->type, type, sizeof(entry->type) - 1);
    if (parse_ble_addr(device_id, entry->ble_addr) == 0) {
        entry->ble_addr_type = 0;
        entry->has_ble_addr = 1;
    }
    entry->connected = 0;
    s_count++;

    if (save_to_nvs() != ESP_OK) {
        s_count--;
        return -1;
    }
    ESP_LOGI(TAG, "Added device: %s (%s, type=%s)", device_id, name, type);
    return 0;
}

int device_store_delete(const char *device_id)
{
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_cache[i].device_id, device_id) == 0) { idx = i; break; }
    }
    if (idx < 0) return -1;

    for (int i = idx; i < s_count - 1; i++) s_cache[i] = s_cache[i + 1];
    s_count--;

    if (save_to_nvs() != ESP_OK) return -1;
    ESP_LOGI(TAG, "Deleted device: %s", device_id);
    return 0;
}

int device_store_edit(const char *device_id, const char *new_name, const char *new_type)
{
    device_entry_t *entry = device_store_find(device_id);
    if (entry == NULL) return -1;

    if (new_name != NULL) strncpy(entry->name, new_name, sizeof(entry->name) - 1);
    if (new_type != NULL) strncpy(entry->type, new_type, sizeof(entry->type) - 1);

    if (save_to_nvs() != ESP_OK) return -1;
    ESP_LOGI(TAG, "Edited device: %s", device_id);
    return 0;
}

const device_entry_t *device_store_list(int *out_count)
{
    if (out_count != NULL) *out_count = s_count;
    return s_cache;
}

device_entry_t *device_store_find(const char *device_id)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_cache[i].device_id, device_id) == 0) return &s_cache[i];
    }
    return NULL;
}

void device_store_set_connected(const char *device_id, int connected)
{
    device_entry_t *entry = device_store_find(device_id);
    if (entry != NULL) entry->connected = connected;
}
