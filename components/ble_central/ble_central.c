#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "ble_central.h"
#include "ble_central_internal.h"
#include "device_store.h"

/* Provided by NimBLE's store/config component. */
void ble_store_config_init(void);

static const char *TAG = "ble_central";

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_ble_host_reset(int reason)
{
    g_ble_host_synced = false;
    ble_central_scan_reset();
    ESP_LOGE(TAG, "NimBLE host reset, reason=%d", reason);

    if (ble_central_lock_connections()) {
        for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
            ble_conn_slot_t *slot = &g_ble_connections[i];
            if (slot->state == BLE_CONN_SLOT_FREE) continue;

            device_store_set_connected(slot->device_id, 0);
            if (slot->forget_requested) {
                memset(slot, 0, sizeof(*slot));
            } else {
                ble_central_reset_runtime_handles(slot);
                slot->state = BLE_CONN_SLOT_IDLE;
            }
        }
        ble_central_unlock_connections();
    }
}

static void on_ble_host_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &g_ble_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not infer own BLE address type: %d", rc);
        return;
    }
    g_ble_host_synced = true;
    ESP_LOGI(TAG, "NimBLE host synced (own_addr_type=%u)",
             g_ble_own_addr_type);
}

int ble_central_init(ble_central_notify_cb_t notify_cb)
{
    if (ble_central_state_init(notify_cb) != 0) return -1;
    ble_central_scan_reset();

    esp_err_t error = nimble_port_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(error));
        return -1;
    }

    ble_hs_cfg.reset_cb = on_ble_host_reset;
    ble_hs_cfg.sync_cb = on_ble_host_sync;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_store_config_init();
    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "BLE Central initialized (max_conn=%d)",
             BLE_CENTRAL_MAX_CONN);
    return 0;
}

int ble_central_connect(const char *device_id, const uint8_t *ble_addr,
                        uint8_t addr_type)
{
    if (device_id == NULL || device_id[0] == '\0' || ble_addr == NULL) return -1;
    if (!g_ble_host_synced) return -2;
    if (!ble_central_lock_connections()) return -1;

    device_entry_t registered_device;
    if (device_store_get(device_id, &registered_device) != 0) {
        ble_central_unlock_connections();
        return -3;
    }

    ble_conn_slot_t *slot = ble_central_find_slot_unlocked(device_id);
    if (slot == NULL) {
        slot = ble_central_allocate_slot_unlocked();
        if (slot == NULL) {
            ble_central_unlock_connections();
            ESP_LOGE(TAG, "No free connection slot (max=%d)",
                     BLE_CENTRAL_MAX_CONN);
            return -1;
        }
        memset(slot, 0, sizeof(*slot));
        ble_central_reset_runtime_handles(slot);
        strlcpy(slot->device_id, device_id, sizeof(slot->device_id));
        slot->state = BLE_CONN_SLOT_IDLE;
    }
    if (slot->state != BLE_CONN_SLOT_IDLE) {
        ble_central_unlock_connections();
        return -1;
    }

    slot->peer_addr.type = addr_type;
    memcpy(slot->peer_addr.val, ble_addr, sizeof(slot->peer_addr.val));
    slot->forget_requested = false;
    slot->state = BLE_CONN_SLOT_CONNECTING;
    slot->last_attempt_ms = esp_timer_get_time() / 1000;
    ble_addr_t peer_addr = slot->peer_addr;
    ble_central_unlock_connections();

    uint16_t interval = ble_central_calculate_conn_interval();
    struct ble_gap_conn_params parameters = {
        .scan_itvl = 16,
        .scan_window = 16,
        .itvl_min = interval,
        .itvl_max = interval,
        .latency = BLE_CONN_LATENCY,
        .supervision_timeout = BLE_CONN_TIMEOUT_UNITS,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    int rc = ble_gap_connect(g_ble_own_addr_type, &peer_addr,
                             BLE_CONNECT_TIMEOUT_MS, &parameters,
                             ble_central_gap_event_handler, slot);
    if (rc != 0) {
        if (ble_central_lock_connections()) {
            if (slot->state == BLE_CONN_SLOT_CONNECTING) {
                slot->state = BLE_CONN_SLOT_IDLE;
            }
            ble_central_unlock_connections();
        }
        ESP_LOGW(TAG, "[%s] ble_gap_connect failed: %d", device_id, rc);
        return -1;
    }

    ESP_LOGI(TAG, "[%s] Connecting (interval=%.1f ms)", device_id,
             interval * 1.25f);
    return 0;
}

int ble_central_disconnect(const char *device_id)
{
    if (device_id == NULL || !ble_central_lock_connections()) return -1;
    ble_conn_slot_t *slot = ble_central_find_slot_unlocked(device_id);
    uint16_t handle = slot != NULL ? slot->conn_handle
                                   : BLE_HS_CONN_HANDLE_NONE;
    ble_central_unlock_connections();
    if (handle == BLE_HS_CONN_HANDLE_NONE) return -1;
    return ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM) == 0 ? 0 : -1;
}

int ble_central_forget_peer(const char *device_id, const uint8_t ble_addr[6],
                            uint8_t ble_addr_type, bool has_ble_addr)
{
    if (device_id == NULL || !ble_central_lock_connections()) return -1;
    ble_conn_slot_t *slot = ble_central_find_slot_unlocked(device_id);
    ble_addr_t peer_address = {0};
    bool has_peer_address = has_ble_addr && ble_addr != NULL;
    if (has_peer_address) {
        peer_address.type = ble_addr_type;
        memcpy(peer_address.val, ble_addr, sizeof(peer_address.val));
    }
    if (slot != NULL) {
        // The runtime connection always knows the freshest peer identity.
        peer_address = slot->peer_addr;
        has_peer_address = true;
    }

    ble_conn_slot_state_t state = BLE_CONN_SLOT_FREE;
    uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
    if (slot != NULL) {
        state = slot->state;
        handle = slot->conn_handle;
        if (state == BLE_CONN_SLOT_IDLE) {
            memset(slot, 0, sizeof(*slot));
        } else {
            slot->forget_requested = true;
        }
    }
    ble_central_unlock_connections();

    if (state == BLE_CONN_SLOT_CONNECTING) {
        ble_gap_conn_cancel();
    } else if (handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    if (!has_peer_address) return 0;
    if (!g_ble_host_synced) {
        ESP_LOGW(TAG, "[%s] BLE host not synced; bond not deleted", device_id);
        return -1;
    }
    int rc = ble_store_util_delete_peer(&peer_address);
    if (rc != 0 && rc != BLE_HS_ENOENT) {
        ESP_LOGE(TAG, "[%s] Could not delete bond: %d", device_id, rc);
        return -1;
    }
    return 0;
}

int ble_central_send_command(const char *device_id, const gw_message_t *msg)
{
    if (device_id == NULL || msg == NULL ||
        !ble_central_lock_connections()) {
        return -1;
    }
    ble_conn_slot_t *slot = ble_central_find_slot_unlocked(device_id);
    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    uint16_t value_handle = 0;
    if (slot != NULL && slot->state == BLE_CONN_SLOT_READY) {
        conn_handle = slot->conn_handle;
        value_handle = slot->command_val_handle;
    }
    ble_central_unlock_connections();
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || value_handle == 0) return -1;

    uint8_t buffer[GW_MSG_MAX_LEN];
    int length = cbor_codec_encode(msg, buffer, sizeof(buffer));
    if (length <= 0) return -1;
    int rc = ble_gattc_write_no_rsp_flat(conn_handle, value_handle, buffer,
                                         length);
    if (rc != 0) {
        ESP_LOGE(TAG, "[%s] GATT write failed: %d", device_id, rc);
        return -1;
    }
    return 0;
}

int ble_central_is_connected(const char *device_id)
{
    if (device_id == NULL || !ble_central_lock_connections()) return 0;
    ble_conn_slot_t *slot = ble_central_find_slot_unlocked(device_id);
    int connected = slot != NULL && slot->state == BLE_CONN_SLOT_READY;
    ble_central_unlock_connections();
    return connected;
}

int ble_central_active_count(void)
{
    if (!ble_central_lock_connections()) return 0;
    int count = ble_central_active_count_unlocked();
    ble_central_unlock_connections();
    return count;
}
