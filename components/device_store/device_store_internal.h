#ifndef DEVICE_STORE_INTERNAL_H
#define DEVICE_STORE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nvs.h"

#include "device_store.h"

// Entry validation/identity helpers (device_store_entry.c).
bool device_store_entry_create(device_entry_t *entry, const char *device_id,
                               const char *name);
bool device_store_entry_edit(device_entry_t *entry, const char *new_name);

// Parses "AA:BB:CC:DD:EE:FF" (or '-' separators) into NimBLE byte order.
// Only used by the legacy schema v1 migration loader; never by create paths.
int device_store_entry_parse_ble_addr(const char *text, uint8_t addr[6]);

// Schema-aware record loaders (device_store_migration.c). One loader per
// stored schema version; the dispatcher refuses to run newer-schema logic
// on older records or vice versa. Returns OK, CORRUPT (skip the record)
// or PERSISTENCE (abort the whole load).
device_store_result_t device_store_migration_load_entry(
    nvs_handle_t handle, uint8_t stored_schema, int index,
    device_entry_t *out_entry);

// NVS backend (device_store_nvs.c).
device_store_result_t device_store_nvs_load(device_entry_t *entries,
                                            size_t capacity,
                                            size_t *out_count);
device_store_result_t device_store_nvs_append(size_t index, size_t new_count,
                                              const device_entry_t *entry);
device_store_result_t device_store_nvs_update(size_t index,
                                              const device_entry_t *entry);
device_store_result_t device_store_nvs_delete(size_t index,
                                              const device_entry_t *compacted,
                                              size_t previous_count);

// Test-only hook: allows device_store_init() to run again so persistence
// tests can reload from NVS without rebooting the target.
void device_store_reset_for_test(void);

#endif /* DEVICE_STORE_INTERNAL_H */
