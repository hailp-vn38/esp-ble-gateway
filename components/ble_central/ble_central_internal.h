#ifndef BLE_CENTRAL_INTERNAL_H
#define BLE_CENTRAL_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "host/ble_gap.h"
#include "host/ble_uuid.h"

#include "ble_central.h"

#define BLE_GATEWAY_SERVICE_UUID         0xABF0
#define BLE_GATEWAY_COMMAND_UUID         0xABF1
#define BLE_GATEWAY_STATUS_UUID          0xABF2

#define BLE_CONN_ITVL_FAST_UNITS         12  /* 15 ms */
#define BLE_CONN_ITVL_MEDIUM_UNITS       24  /* 30 ms */
#define BLE_CONN_ITVL_BUSY_UNITS         40  /* 50 ms */
#define BLE_CONN_LATENCY                  0
#define BLE_CONN_TIMEOUT_UNITS          200  /* 2 seconds */
#define BLE_CONNECT_TIMEOUT_MS        10000
#define BLE_DISCOVERY_TIMEOUT_MS      10000
#define BLE_RECONNECT_INTERVAL_MS      8000
#define BLE_SUPERVISOR_TICK_MS         1000

typedef enum {
    BLE_CONN_SLOT_FREE = 0,
    BLE_CONN_SLOT_IDLE,
    BLE_CONN_SLOT_CONNECTING,
    BLE_CONN_SLOT_SECURING,
    BLE_CONN_SLOT_DISCOVERING,
    BLE_CONN_SLOT_READY,
} ble_conn_slot_state_t;

typedef struct {
    ble_conn_slot_state_t state;
    bool forget_requested;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    ble_addr_t peer_addr;
    uint16_t conn_handle;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t command_val_handle;
    uint16_t status_val_handle;
    uint16_t status_cccd_handle;
    int64_t last_attempt_ms;
    int64_t discovery_started_ms;
} ble_conn_slot_t;

extern ble_conn_slot_t g_ble_connections[BLE_CENTRAL_MAX_CONN];
extern volatile bool g_ble_host_synced;
extern uint8_t g_ble_own_addr_type;
extern const ble_uuid16_t g_ble_gateway_service_uuid;

int ble_central_state_init(ble_central_notify_cb_t notify_cb);
bool ble_central_lock_connections(void);
void ble_central_unlock_connections(void);
ble_conn_slot_t *ble_central_find_slot_unlocked(const char *device_id);
ble_conn_slot_t *ble_central_allocate_slot_unlocked(void);
void ble_central_reset_runtime_handles(ble_conn_slot_t *slot);
int ble_central_active_count_unlocked(void);
uint16_t ble_central_calculate_conn_interval(void);
ble_central_notify_cb_t ble_central_notify_callback(void);

void ble_central_abort_discovery(ble_conn_slot_t *slot, const char *reason);
void ble_central_start_gatt_discovery(ble_conn_slot_t *slot);
int ble_central_gap_event_handler(struct ble_gap_event *event, void *arg);
void ble_central_scan_reset(void);

#endif /* BLE_CENTRAL_INTERNAL_H */
