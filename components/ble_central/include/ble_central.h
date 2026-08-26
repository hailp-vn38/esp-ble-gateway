#ifndef BLE_CENTRAL_H
#define BLE_CENTRAL_H

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "cbor_codec.h"

#define BLE_CENTRAL_MAX_CONN   CONFIG_BT_NIMBLE_MAX_CONNECTIONS

typedef enum {
    BLE_CENTRAL_OK = 0,
    BLE_CENTRAL_ERR_INVALID_ARG = -1,
    BLE_CENTRAL_ERR_NOT_READY = -2,
    BLE_CENTRAL_ERR_NOT_FOUND = -3,
    BLE_CENTRAL_ERR_NO_SLOT = -4,
    BLE_CENTRAL_ERR_BUSY = -5,
    BLE_CENTRAL_ERR_CONNECT_IN_PROGRESS = -6,
    BLE_CENTRAL_ERR_NOT_CONNECTED = -7,
    BLE_CENTRAL_ERR_ENCODE = -8,
    BLE_CENTRAL_ERR_MESSAGE_TOO_LARGE = -9,
    BLE_CENTRAL_ERR_STACK = -10,
    BLE_CENTRAL_ERR_NO_RESOURCE = -11,
    BLE_CENTRAL_ERR_STATE = -12,
} ble_central_err_t;

typedef void (*ble_central_notify_cb_t)(const char *device_id, const gw_message_t *msg);

// Runtime connection state of one device, owned entirely by BLE Central.
typedef enum {
    BLE_CENTRAL_DEVICE_OFFLINE = 0,
    BLE_CENTRAL_DEVICE_CONNECTING,
    BLE_CENTRAL_DEVICE_CONNECTED,
    BLE_CENTRAL_DEVICE_BACKOFF,
    BLE_CENTRAL_DEVICE_REMOVING,
} ble_central_device_state_t;

typedef struct {
    bool known;    // Device has a runtime entry (registered for reconnect).
    bool connected;// An ACL link exists for the device.
    bool ready;    // Link secured, GATT discovered and notifications enabled.
    ble_central_device_state_t state;
} ble_central_device_status_t;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    char name[32];
} ble_scan_result_t;

typedef void (*ble_central_scan_result_cb_t)(const ble_scan_result_t *result);

int ble_central_init(ble_central_notify_cb_t notify_cb);
int ble_central_connect(const char *device_id, const uint8_t *ble_addr, uint8_t addr_type);
int ble_central_disconnect(const char *device_id);
// Tears down any runtime connection for device_id and deletes the stored
// bond. Peer identity must be supplied by the caller (snapshot taken before
// the device_store entry is removed); when has_ble_addr is false only the
// runtime connection is cleaned up. Returns 0 on success (including the
// idempotent "no bond existed" case), -1 if the bond could not be deleted.
int ble_central_forget_peer(const char *device_id, const uint8_t ble_addr[6],
                            uint8_t ble_addr_type, bool has_ble_addr);
int ble_central_send_command(const char *device_id, const gw_message_t *msg);
int ble_central_is_connected(const char *device_id);
int ble_central_active_count(void);
// Copy-out runtime status query. This (not device_store) is the source of
// truth for connection state; merge with device_store_snapshot() at the
// presentation layer.
ble_central_err_t ble_central_get_device_status(
    const char *device_id, ble_central_device_status_t *out_status);
int ble_central_scan_start(ble_central_scan_result_cb_t scan_result_cb);
int ble_central_scan_stop(void);
int ble_central_is_scanning(void);
int ble_central_start_reconnect_supervisor(void);
void ble_central_stop_reconnect_supervisor(void);

#endif // BLE_CENTRAL_H
