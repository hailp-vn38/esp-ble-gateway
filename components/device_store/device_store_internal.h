#ifndef DEVICE_STORE_INTERNAL_H
#define DEVICE_STORE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "device_store.h"

bool device_store_entry_create(device_entry_t *entry, const char *device_id,
                               const char *name, const char *type);
bool device_store_entry_edit(device_entry_t *entry, const char *new_name,
                             const char *new_type);
int device_store_entry_parse_ble_addr(const char *text, uint8_t addr[6]);

esp_err_t device_store_nvs_load(device_entry_t *entries, size_t capacity,
                                int *out_count);
esp_err_t device_store_nvs_append(int index, int new_count,
                                  const device_entry_t *entry);
esp_err_t device_store_nvs_update(int index, const device_entry_t *entry);
esp_err_t device_store_nvs_delete(int index,
                                  const device_entry_t *compacted_entries,
                                  int previous_count);

#endif /* DEVICE_STORE_INTERNAL_H */
