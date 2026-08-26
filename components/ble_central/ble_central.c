#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
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
    char device_ids[DEVICE_STORE_MAX_DEVICES][GW_MSG_DEVICE_ID_LEN];
    size_t mirror_count =
        ble_state_handle_host_reset(device_ids, DEVICE_STORE_MAX_DEVICES);

    ble_host_set_ready(false);
    ble_central_scan_reset();
    ESP_LOGE(TAG, "NimBLE host reset, reason=%d (cleared %u link(s))", reason,
             (unsigned)mirror_count);
}

static void on_ble_host_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &g_ble_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not infer own BLE address type: %d", rc);
        return;
    }
    ble_host_set_ready(true);
    ESP_LOGI(TAG, "NimBLE host synced (own_addr_type=%u)",
             g_ble_own_addr_type);
}

int ble_central_init(ble_central_notify_cb_t notify_cb)
{
    if (ble_central_state_init(notify_cb) != 0) return -1;
    if (ble_central_runtime_init() != BLE_CENTRAL_OK) return -1;
    if (ble_central_identity_init() != 0) {
        ESP_LOGE(TAG, "Identity persistence worker failed to start");
        return -1;
    }
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
    if (device_id == NULL || device_id[0] == '\0' || ble_addr == NULL) {
        return BLE_CENTRAL_ERR_INVALID_ARG;
    }
    if (!ble_host_is_ready()) return BLE_CENTRAL_ERR_NOT_READY;

    device_entry_t registered_device;
    if (device_store_get(device_id, &registered_device) != 0) {
        return BLE_CENTRAL_ERR_NOT_FOUND;
    }

    ble_addr_t peer_addr;
    peer_addr.type = addr_type;
    memcpy(peer_addr.val, ble_addr, sizeof(peer_addr.val));

    int device_index = ble_runtime_find_or_register(device_id, &peer_addr);
    if (device_index < 0) return device_index;

    return ble_connection_start(device_index);
}

int ble_central_disconnect(const char *device_id)
{
    if (device_id == NULL) return BLE_CENTRAL_ERR_INVALID_ARG;

    int device_index = ble_runtime_find(device_id);
    if (device_index < 0) return BLE_CENTRAL_ERR_NOT_FOUND;

    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    if (!ble_state_lock()) return BLE_CENTRAL_ERR_STATE;

    do {
        int slot_index = g_ble_devices[device_index].connection_slot;
        if (slot_index < 0 || slot_index >= BLE_CENTRAL_MAX_CONN) break;

        ble_conn_slot_t *slot = &g_ble_connections[slot_index];
        if (slot->state == BLE_CONN_FREE ||
            slot->conn_handle == BLE_HS_CONN_HANDLE_NONE) break;

        conn_handle = slot->conn_handle;
        slot->state = BLE_CONN_DISCONNECTING;
    } while (0);
    ble_state_unlock();

    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_CENTRAL_ERR_NOT_CONNECTED;
    }

    int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return rc == 0 ? BLE_CENTRAL_OK : BLE_CENTRAL_ERR_STACK;
}

int ble_central_forget_peer(const char *device_id, const uint8_t ble_addr[6],
                            uint8_t ble_addr_type, bool has_ble_addr)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return BLE_CENTRAL_ERR_INVALID_ARG;
    }

    bool has_bond_addr = false;
    ble_addr_t bond_addr = {0};
    bool need_cancel = false;
    bool need_terminate = false;
    uint16_t terminate_handle = BLE_HS_CONN_HANDLE_NONE;
    bool had_link = false;
    int runtime_index = ble_runtime_find(device_id);

    if (!ble_state_lock()) return BLE_CENTRAL_ERR_STATE;
    if (runtime_index >= 0) {
        ble_device_runtime_t *dev = &g_ble_devices[runtime_index];
        if (dev->has_peer_addr) {
            bond_addr = dev->peer_addr;
            has_bond_addr = true;
        }
        if (dev->state != BLE_DEVICE_REMOVING) {
            dev->reconnect_enabled = false;
            dev->state = BLE_DEVICE_REMOVING;
        }

        int slot_index = dev->connection_slot;
        if (slot_index >= 0 && slot_index < BLE_CENTRAL_MAX_CONN &&
            g_ble_connections[slot_index].state != BLE_CONN_FREE) {
            ble_conn_slot_t *slot = &g_ble_connections[slot_index];
            had_link = true;
            if (slot->state == BLE_CONN_CONNECTING) {
                need_cancel = true;
            } else if (slot->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                need_terminate = true;
                terminate_handle = slot->conn_handle;
                slot->state = BLE_CONN_DISCONNECTING;
            }
        }
    }
    ble_state_unlock();

    if (!has_bond_addr && has_ble_addr && ble_addr != NULL) {
        bond_addr.type = ble_addr_type;
        memcpy(bond_addr.val, ble_addr, sizeof(bond_addr.val));
        has_bond_addr = true;
    }

    if (need_cancel) {
        ble_gap_conn_cancel();
    } else if (need_terminate) {
        ble_gap_terminate(terminate_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    if (!had_link) {
        if (runtime_index >= 0) {
            ble_runtime_finalize_remove(runtime_index);
        }
        if (!has_bond_addr) return BLE_CENTRAL_OK;
        if (!ble_host_is_ready()) {
            ESP_LOGW(TAG, "[%s] BLE host not synced; bond not deleted",
                     device_id);
            return BLE_CENTRAL_ERR_STACK;
        }
        int rc = ble_store_util_delete_peer(&bond_addr);
        if (rc != 0 && rc != BLE_HS_ENOENT) {
            ESP_LOGE(TAG, "[%s] Could not delete bond: %d", device_id, rc);
            return BLE_CENTRAL_ERR_STACK;
        }
    }

    ESP_LOGI(TAG, "[%s] Forget requested (%s)", device_id,
             need_cancel ? "canceling connect"
                         : need_terminate ? "terminating link"
                                          : "no active link");
    return BLE_CENTRAL_OK;
}

int ble_central_send_command(const char *device_id, const gw_message_t *msg)
{
    if (device_id == NULL || msg == NULL) return BLE_CENTRAL_ERR_INVALID_ARG;

    int device_index = ble_runtime_find(device_id);
    if (device_index < 0) return BLE_CENTRAL_ERR_NOT_FOUND;

    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    uint16_t value_handle = 0;
    uint16_t cached_mtu = 23;
    if (!ble_state_lock()) return BLE_CENTRAL_ERR_STATE;

    do {
        int slot_index = g_ble_devices[device_index].connection_slot;
        if (slot_index < 0 || slot_index >= BLE_CENTRAL_MAX_CONN) break;

        ble_conn_slot_t *slot = &g_ble_connections[slot_index];
        if (slot->state == BLE_CONN_FREE) break;
        if (slot->state != BLE_CONN_READY ||
            slot->conn_handle == BLE_HS_CONN_HANDLE_NONE ||
            slot->command_val_handle == 0) {
            break;
        }

        conn_handle = slot->conn_handle;
        value_handle = slot->command_val_handle;
        cached_mtu = slot->mtu;
    } while (0);
    ble_state_unlock();

    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || value_handle == 0) {
        return BLE_CENTRAL_ERR_NOT_CONNECTED;
    }

    uint16_t mtu = cached_mtu;
    uint16_t max_payload = mtu > 3 ? (uint16_t)(mtu - 3) : 0;

    uint8_t buffer[GW_MSG_MAX_LEN];
    int length = cbor_codec_encode(msg, buffer, sizeof(buffer));
    if (length <= 0) return BLE_CENTRAL_ERR_ENCODE;
    if ((uint16_t)length > max_payload) {
        ble_central_metrics_mtu_reject();
        ESP_LOGE(TAG, "[%s] Payload %d exceeds MTU payload %u", device_id,
                 length, max_payload);
        return BLE_CENTRAL_ERR_MESSAGE_TOO_LARGE;
    }

    int rc = ble_gattc_write_no_rsp_flat(conn_handle, value_handle, buffer,
                                         length);
    if (rc != 0) {
        ESP_LOGE(TAG, "[%s] GATT write failed: %d", device_id, rc);
        return BLE_CENTRAL_ERR_STACK;
    }
    return BLE_CENTRAL_OK;
}

int ble_central_is_connected(const char *device_id)
{
    if (device_id == NULL) return 0;

    int device_index = ble_runtime_find(device_id);
    if (device_index < 0) return 0;

    int connected = 0;
    if (!ble_state_lock()) return 0;

    int slot_index = g_ble_devices[device_index].connection_slot;
    if (slot_index >= 0 && slot_index < BLE_CENTRAL_MAX_CONN &&
        g_ble_connections[slot_index].state == BLE_CONN_READY) {
        connected = 1;
    }

    ble_state_unlock();
    return connected;
}

int ble_central_active_count(void)
{
    if (!ble_state_lock()) return 0;
    int count = ble_central_active_count_unlocked();
    ble_state_unlock();
    return count;
}

ble_central_err_t ble_central_get_device_status(
    const char *device_id, ble_central_device_status_t *out_status)
{
    if (device_id == NULL || out_status == NULL) {
        return BLE_CENTRAL_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (!ble_state_lock()) return BLE_CENTRAL_ERR_STATE;

    ble_central_err_t result = BLE_CENTRAL_ERR_NOT_FOUND;
    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        const ble_device_runtime_t *dev = &g_ble_devices[i];
        if (!dev->in_use || strcmp(dev->device_id, device_id) != 0) continue;

        out_status->known = true;
        // The runtime and public state enums are intentionally aligned.
        out_status->state = (ble_central_device_state_t)dev->state;
        result = BLE_CENTRAL_OK;

        int slot_index = dev->connection_slot;
        if (slot_index >= 0 && slot_index < BLE_CENTRAL_MAX_CONN) {
            const ble_conn_slot_t *slot = &g_ble_connections[slot_index];
            switch (slot->state) {
            case BLE_CONN_SECURING:
            case BLE_CONN_DISCOVERING:
            case BLE_CONN_READY:
            case BLE_CONN_DISCONNECTING:
                out_status->connected = true;
                break;
            default:
                break;
            }
            out_status->ready = slot->state == BLE_CONN_READY;
        }
        break;
    }

    ble_state_unlock();
    return result;
}
