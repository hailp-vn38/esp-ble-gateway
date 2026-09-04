#ifndef DEVICE_MANAGEMENT_H
#define DEVICE_MANAGEMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_schema.h"
#include "device_types.h"

typedef enum {
    DEVICE_MGMT_OK = 0,
    DEVICE_MGMT_INVALID_ARG,
    DEVICE_MGMT_NOT_FOUND,
    DEVICE_MGMT_CONFLICT,
    DEVICE_MGMT_CAPACITY,
    DEVICE_MGMT_BUSY,
    DEVICE_MGMT_DEGRADED,
    DEVICE_MGMT_INTERNAL,
} device_mgmt_status_t;

typedef struct {
    device_id_t device_id;
    device_name_t name;
    bool has_ble_identity;
    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
} device_mgmt_add_request_t;

typedef struct {
    device_mgmt_status_t status;
    bool persisted;
    bool connect_requested;
} device_mgmt_add_result_t;

typedef struct {
    device_id_t device_id;
    device_name_t name;
} device_mgmt_edit_request_t;

typedef struct {
    device_mgmt_status_t status;
    bool updated;
} device_mgmt_edit_result_t;

typedef struct {
    device_mgmt_status_t status;
    bool command_cancel_requested;
    bool schema_forgotten;
    bool state_forgotten;
    bool ble_peer_forgotten;
    bool store_deleted;
} device_mgmt_delete_result_t;

typedef struct {
    device_id_t device_id;
    device_name_t name;
    bool connected;
    bool ready;
    bool has_ble_identity;
    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    bool schema_available;
    device_schema_state_t schema_state;
    uint32_t schema_revision;
    uint8_t feature_count;
    uint8_t writable_feature_count;
} device_inventory_entry_t;

/*
 * All operations are synchronous bounded metadata operations. They never wait
 * for a BLE connection or command ACK and create no worker task. A BLE connect
 * from add is only a best-effort trigger reflected by connect_requested.
 */
device_mgmt_add_result_t device_management_add(
    const device_mgmt_add_request_t *request);
device_mgmt_edit_result_t device_management_edit(
    const device_mgmt_edit_request_t *request);
/*
 * Delete order is command cancel, schema forget, state forget, BLE peer
 * forget, store delete, then lifecycle publish. Schema failure aborts the
 * destructive remainder; BLE/store cleanup failures return DEGRADED with
 * per-step flags describing the resulting state.
 */
device_mgmt_delete_result_t device_management_delete(const char *device_id);

device_mgmt_status_t device_management_snapshot(
    device_inventory_entry_t *out_entries, size_t capacity,
    size_t *out_count);

#endif /* DEVICE_MANAGEMENT_H */
