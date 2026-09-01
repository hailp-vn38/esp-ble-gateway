#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "device_store_internal.h"

static const char *TAG = "device_store";

/* Buffer for reading the legacy type_N field during v2 migration. */
#define DEVICE_TYPE_MAX_LEN_V2 16

// Classifies an NVS read failure for a key whose absence means the record
// is incomplete rather than the storage being broken.
static device_store_result_t classify_key_error(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return DEVICE_STORE_OK;
    case ESP_ERR_NVS_NOT_FOUND:
    case ESP_ERR_NVS_TYPE_MISMATCH:
    case ESP_ERR_NVS_INVALID_LENGTH:
        return DEVICE_STORE_ERR_CORRUPT;
    default:
        return DEVICE_STORE_ERR_PERSISTENCE;
    }
}

static device_store_result_t load_required_str_v2(nvs_handle_t handle,
                                                  const char *key, char *out,
                                                  size_t out_size)
{
    size_t length = out_size;
    esp_err_t err = nvs_get_str(handle, key, out, &length);
    if (err == ESP_OK && (length == 0 || out[0] == '\0')) {
        return DEVICE_STORE_ERR_CORRUPT;
    }
    return classify_key_error(err);
}

// Schema v1 records: name/type were optional with fallbacks and a MAC-shaped
// device_id doubled as the BLE address. This leniency is only legal here —
// never on current-schema data (refactor plan §8).
static device_store_result_t load_entry_v1(nvs_handle_t handle, int index,
                                           device_entry_t *entry)
{
    char key[16];
    memset(entry, 0, sizeof(*entry));

    snprintf(key, sizeof(key), "id_%d", index);
    device_store_result_t result =
        load_required_str_v2(handle, key, entry->device_id, sizeof(entry->device_id));
    if (result != DEVICE_STORE_OK) return result;

    snprintf(key, sizeof(key), "name_%d", index);
    size_t length = sizeof(entry->name);
    if (nvs_get_str(handle, key, entry->name, &length) != ESP_OK) {
        strlcpy(entry->name, entry->device_id, sizeof(entry->name));
    }

    snprintf(key, sizeof(key), "type_%d", index);
    char type_buf[DEVICE_TYPE_MAX_LEN_V2];
    length = sizeof(type_buf);
    if (nvs_get_str(handle, key, type_buf, &length) != ESP_OK) {
        /* type_N missing in legacy record — acceptable. */
    }

    snprintf(key, sizeof(key), "addr_%d", index);
    size_t address_len = sizeof(entry->ble_addr);
    if (nvs_get_blob(handle, key, entry->ble_addr, &address_len) == ESP_OK &&
        address_len == sizeof(entry->ble_addr)) {
        snprintf(key, sizeof(key), "atype_%d", index);
        uint8_t addr_type = 0;
        (void)nvs_get_u8(handle, key, &addr_type);
        entry->ble_addr_type = addr_type;
        entry->has_ble_identity = true;
    } else if (device_store_entry_parse_ble_addr(entry->device_id,
                                                 entry->ble_addr) == 0) {
        /* Legacy migration: old records used the MAC as device_id. */
        entry->ble_addr_type = 0;
        entry->has_ble_identity = true;
    }
    return DEVICE_STORE_OK;
}

// Schema v2 records: id/name/type are required; addr/atype must be
// either both present or both absent.
static device_store_result_t load_entry_v2(nvs_handle_t handle, int index,
                                           device_entry_t *entry)
{
    char key[16];
    memset(entry, 0, sizeof(*entry));

    snprintf(key, sizeof(key), "id_%d", index);
    device_store_result_t result =
        load_required_str_v2(handle, key, entry->device_id, sizeof(entry->device_id));
    if (result != DEVICE_STORE_OK) return result;

    snprintf(key, sizeof(key), "name_%d", index);
    result = load_required_str_v2(handle, key, entry->name, sizeof(entry->name));
    if (result != DEVICE_STORE_OK) return result;

    /* type_N is required in v2 but discarded — the field was removed in v3. */
    snprintf(key, sizeof(key), "type_%d", index);
    char type_buf[DEVICE_TYPE_MAX_LEN_V2];
    result = load_required_str_v2(handle, key, type_buf, sizeof(type_buf));
    if (result != DEVICE_STORE_OK) return result;

    snprintf(key, sizeof(key), "addr_%d", index);
    size_t address_len = sizeof(entry->ble_addr);
    esp_err_t addr_err = nvs_get_blob(handle, key, entry->ble_addr, &address_len);

    snprintf(key, sizeof(key), "atype_%d", index);
    uint8_t addr_type = 0;
    esp_err_t type_err = nvs_get_u8(handle, key, &addr_type);

    if (addr_err == ESP_OK && type_err == ESP_OK) {
        if (address_len != sizeof(entry->ble_addr)) return DEVICE_STORE_ERR_CORRUPT;
        entry->ble_addr_type = addr_type;
        entry->has_ble_identity = true;
    } else if (addr_err != ESP_ERR_NVS_NOT_FOUND ||
               type_err != ESP_ERR_NVS_NOT_FOUND) {
        // Half-written identity pair or wrong value types: treat the record
        // as corrupt instead of guessing.
        return DEVICE_STORE_ERR_CORRUPT;
    }
    return DEVICE_STORE_OK;
}

// Schema v3: device-level type removed.  id/name are required; addr/atype
// must be either both present or both absent.
static device_store_result_t load_entry_v3(nvs_handle_t handle, int index,
                                           device_entry_t *entry)
{
    char key[16];
    memset(entry, 0, sizeof(*entry));

    snprintf(key, sizeof(key), "id_%d", index);
    device_store_result_t result =
        load_required_str_v2(handle, key, entry->device_id, sizeof(entry->device_id));
    if (result != DEVICE_STORE_OK) return result;

    snprintf(key, sizeof(key), "name_%d", index);
    result = load_required_str_v2(handle, key, entry->name, sizeof(entry->name));
    if (result != DEVICE_STORE_OK) return result;

    snprintf(key, sizeof(key), "addr_%d", index);
    size_t address_len = sizeof(entry->ble_addr);
    esp_err_t addr_err = nvs_get_blob(handle, key, entry->ble_addr, &address_len);

    snprintf(key, sizeof(key), "atype_%d", index);
    uint8_t addr_type = 0;
    esp_err_t type_err = nvs_get_u8(handle, key, &addr_type);

    if (addr_err == ESP_OK && type_err == ESP_OK) {
        if (address_len != sizeof(entry->ble_addr)) return DEVICE_STORE_ERR_CORRUPT;
        entry->ble_addr_type = addr_type;
        entry->has_ble_identity = true;
    } else if (addr_err != ESP_ERR_NVS_NOT_FOUND ||
               type_err != ESP_ERR_NVS_NOT_FOUND) {
        return DEVICE_STORE_ERR_CORRUPT;
    }
    return DEVICE_STORE_OK;
}

device_store_result_t device_store_migration_load_entry(
    nvs_handle_t handle, uint8_t stored_schema, int index,
    device_entry_t *out_entry)
{
    switch (stored_schema) {
    case 1:
        return load_entry_v1(handle, index, out_entry);
    case 2:
        return load_entry_v2(handle, index, out_entry);
    case DEVICE_STORE_SCHEMA_VERSION:
        return load_entry_v3(handle, index, out_entry);
    default:
        // The caller rejects unknown/future schemas before reaching here.
        ESP_LOGE(TAG, "No loader for schema v%u", stored_schema);
        return DEVICE_STORE_ERR_SCHEMA_TOO_NEW;
    }
}
