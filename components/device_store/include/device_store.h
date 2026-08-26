#ifndef DEVICE_STORE_H
#define DEVICE_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define DEVICE_STORE_MAX_DEVICES   16
#define DEVICE_ID_MAX_LEN          32
#define DEVICE_NAME_MAX_LEN        32
#define DEVICE_TYPE_MAX_LEN        16
#define DEVICE_STORE_SCHEMA_VERSION 2

// Highest BLE address type accepted by the store (NimBLE: 0=public,
// 1=random static/private, 2=public ID, 3=random ID).
#define DEVICE_STORE_BLE_ADDR_TYPE_MAX 3

// Typed result contract. OK is the only success value; every other code
// identifies a distinct failure class so callers can react precisely.
typedef enum {
    DEVICE_STORE_OK = 0,
    DEVICE_STORE_ERR_INVALID_ARG,
    DEVICE_STORE_ERR_NOT_FOUND,
    DEVICE_STORE_ERR_DUPLICATE_ID,
    DEVICE_STORE_ERR_DUPLICATE_BLE_IDENTITY,
    DEVICE_STORE_ERR_FULL,
    DEVICE_STORE_ERR_BUSY,
    DEVICE_STORE_ERR_PERSISTENCE,
    DEVICE_STORE_ERR_CORRUPT,
    DEVICE_STORE_ERR_SCHEMA_TOO_NEW,
    DEVICE_STORE_ERR_BUFFER_TOO_SMALL,
    DEVICE_STORE_ERR_CAPACITY_EXCEEDED,
    DEVICE_STORE_ERR_INVALID_STATE,
} device_store_result_t;

// Persistent configuration of one device. This is a pure value type:
// it carries no runtime state (connection, GATT handles, MTU live in
// ble_central) and is never returned as a pointer into internal storage.
typedef struct {
    char device_id[DEVICE_ID_MAX_LEN];
    char name[DEVICE_NAME_MAX_LEN];
    char type[DEVICE_TYPE_MAX_LEN];

    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    bool has_ble_identity;
} device_entry_t;

// Single-shot lifecycle. A second call returns DEVICE_STORE_ERR_INVALID_STATE;
// use a fresh boot or the test-only reset hook to reload.
device_store_result_t device_store_init(void);

device_store_result_t device_store_add(const char *device_id, const char *name,
                                       const char *type);

device_store_result_t device_store_delete(const char *device_id);

device_store_result_t device_store_edit(const char *device_id,
                                        const char *new_name,
                                        const char *new_type);

// Copy-out read: never exposes internal cache pointers.
device_store_result_t device_store_get(const char *device_id,
                                       device_entry_t *out_entry);

// Copy-out bulk read. Never truncates silently:
//   capacity >= count  -> OK, copies everything, *out_count = count
//   capacity <  count  -> DEVICE_STORE_ERR_BUFFER_TOO_SMALL, no partial copy,
//                         *out_count = required count
//   out_entries == NULL && capacity == 0 -> query mode, *out_count = count
device_store_result_t device_store_snapshot(device_entry_t *out_entries,
                                            size_t capacity,
                                            size_t *out_count);

// Explicit transport-identity setter. The caller must supply the address
// type; logical device_id is never parsed into a BLE address by the store.
// Rejects a canonical identity that already belongs to another device.
device_store_result_t device_store_set_ble_identity(
    const char *device_id, const uint8_t ble_addr[6], uint8_t addr_type);

#endif // DEVICE_STORE_H
