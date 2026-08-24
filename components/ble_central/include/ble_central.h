#ifndef BLE_CENTRAL_H
#define BLE_CENTRAL_H

#include <stdint.h>
#include "sdkconfig.h"
#include "cbor_codec.h"

#define BLE_CENTRAL_MAX_CONN   CONFIG_BT_NIMBLE_MAX_CONNECTIONS

typedef void (*ble_central_notify_cb_t)(const char *device_id, const gw_message_t *msg);

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
int ble_central_forget_device(const char *device_id);
int ble_central_send_command(const char *device_id, const gw_message_t *msg);
int ble_central_is_connected(const char *device_id);
int ble_central_active_count(void);
int ble_central_scan_start(ble_central_scan_result_cb_t scan_result_cb);
int ble_central_scan_stop(void);
int ble_central_is_scanning(void);
int ble_central_start_reconnect_supervisor(void);
void ble_central_stop_reconnect_supervisor(void);

#endif // BLE_CENTRAL_H
