#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"

#include "ble_central_internal.h"

ble_conn_slot_t g_ble_connections[BLE_CENTRAL_MAX_CONN];
volatile bool g_ble_host_synced;
uint8_t g_ble_own_addr_type;
const ble_uuid16_t g_ble_gateway_service_uuid =
    BLE_UUID16_INIT(BLE_GATEWAY_SERVICE_UUID);

static SemaphoreHandle_t s_connection_mutex;
static ble_central_notify_cb_t s_notify_cb;

int ble_central_state_init(ble_central_notify_cb_t notify_cb)
{
    if (s_connection_mutex == NULL) {
        s_connection_mutex = xSemaphoreCreateMutex();
    }
    if (s_connection_mutex == NULL) return -1;

    memset(g_ble_connections, 0, sizeof(g_ble_connections));
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        ble_central_reset_runtime_handles(&g_ble_connections[i]);
    }
    s_notify_cb = notify_cb;
    g_ble_host_synced = false;
    return 0;
}

bool ble_central_lock_connections(void)
{
    return s_connection_mutex != NULL &&
           xSemaphoreTake(s_connection_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

void ble_central_unlock_connections(void)
{
    xSemaphoreGive(s_connection_mutex);
}

ble_conn_slot_t *ble_central_find_slot_unlocked(const char *device_id)
{
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if (g_ble_connections[i].state != BLE_CONN_SLOT_FREE &&
            strcmp(g_ble_connections[i].device_id, device_id) == 0) {
            return &g_ble_connections[i];
        }
    }
    return NULL;
}

ble_conn_slot_t *ble_central_allocate_slot_unlocked(void)
{
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if (g_ble_connections[i].state == BLE_CONN_SLOT_FREE) {
            return &g_ble_connections[i];
        }
    }
    return NULL;
}

void ble_central_reset_runtime_handles(ble_conn_slot_t *slot)
{
    slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    slot->service_start_handle = 0;
    slot->service_end_handle = 0;
    slot->command_val_handle = 0;
    slot->status_val_handle = 0;
    slot->status_cccd_handle = 0;
    slot->discovery_started_ms = 0;
}

int ble_central_active_count_unlocked(void)
{
    int count = 0;
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if (g_ble_connections[i].conn_handle != BLE_HS_CONN_HANDLE_NONE) count++;
    }
    return count;
}

uint16_t ble_central_calculate_conn_interval(void)
{
    int active = 0;
    if (ble_central_lock_connections()) {
        active = ble_central_active_count_unlocked();
        ble_central_unlock_connections();
    }
    if (active < 3) return BLE_CONN_ITVL_FAST_UNITS;
    if (active < 6) return BLE_CONN_ITVL_MEDIUM_UNITS;
    return BLE_CONN_ITVL_BUSY_UNITS;
}

ble_central_notify_cb_t ble_central_notify_callback(void)
{
    return s_notify_cb;
}
