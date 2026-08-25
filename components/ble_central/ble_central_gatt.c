#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

#include "ble_central_internal.h"
#include "device_store.h"

static const char *TAG = "ble_central_gatt";

static int on_service_discovered(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service,
                                 void *arg);

void ble_central_abort_discovery(ble_conn_slot_t *slot, const char *reason)
{
    ESP_LOGE(TAG, "[%s] GATT discovery failed: %s", slot->device_id, reason);
    if (slot->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(slot->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void ble_central_start_gatt_discovery(ble_conn_slot_t *slot)
{
    if (slot == NULL) return;
    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    if (ble_central_lock_connections()) {
        if (slot->state == BLE_CONN_SLOT_SECURING) {
            slot->state = BLE_CONN_SLOT_DISCOVERING;
            slot->discovery_started_ms = esp_timer_get_time() / 1000;
            conn_handle = slot->conn_handle;
        }
        ble_central_unlock_connections();
    }
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    int rc = ble_gattc_disc_svc_by_uuid(conn_handle,
                                        &g_ble_gateway_service_uuid.u,
                                        on_service_discovered, slot);
    if (rc != 0) {
        ble_central_abort_discovery(slot, "failed to start service discovery");
    }
}

static int on_subscribe_write(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    ble_conn_slot_t *slot = arg;
    if (slot == NULL || slot->conn_handle != conn_handle) return 0;
    if (error->status != 0) {
        ble_central_abort_discovery(slot, "could not write CCCD");
        return 0;
    }

    bool ready = false;
    if (ble_central_lock_connections()) {
        if (slot->conn_handle == conn_handle) slot->state = BLE_CONN_SLOT_READY;
        ready = slot->conn_handle == conn_handle;
        ble_central_unlock_connections();
    }
    if (!ready) return 0;

    device_store_set_connected(slot->device_id, 1);
    ESP_LOGI(TAG, "[%s] GATT ready (command=%u status=%u cccd=%u)",
             slot->device_id, slot->command_val_handle, slot->status_val_handle,
             slot->status_cccd_handle);
    return 0;
}

static int on_descriptor_discovered(uint16_t conn_handle,
                                    const struct ble_gatt_error *error,
                                    uint16_t chr_val_handle,
                                    const struct ble_gatt_dsc *descriptor,
                                    void *arg)
{
    ble_conn_slot_t *slot = arg;
    if (slot == NULL || slot->conn_handle != conn_handle) return 0;

    if (error->status == 0 && descriptor != NULL) {
        if (chr_val_handle == slot->status_val_handle &&
            ble_uuid_cmp(&descriptor->uuid.u,
                         BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            if (ble_central_lock_connections()) {
                if (slot->conn_handle == conn_handle) {
                    slot->status_cccd_handle = descriptor->handle;
                }
                ble_central_unlock_connections();
            }
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ble_central_abort_discovery(slot, "descriptor discovery error");
        return 0;
    }
    if (slot->status_cccd_handle == 0) {
        ble_central_abort_discovery(slot, "STATUS CCCD not found");
        return 0;
    }

    const uint8_t notify_enable[] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn_handle, slot->status_cccd_handle,
                                  notify_enable, sizeof(notify_enable),
                                  on_subscribe_write, slot);
    if (rc != 0) {
        ble_central_abort_discovery(slot, "failed to start CCCD write");
    }
    return 0;
}

static int on_characteristic_discovered(
    uint16_t conn_handle, const struct ble_gatt_error *error,
    const struct ble_gatt_chr *characteristic, void *arg)
{
    ble_conn_slot_t *slot = arg;
    if (slot == NULL || slot->conn_handle != conn_handle) return 0;

    if (error->status == 0 && characteristic != NULL) {
        uint16_t uuid = ble_uuid_u16(&characteristic->uuid.u);
        if (ble_central_lock_connections()) {
            if (slot->conn_handle == conn_handle &&
                uuid == BLE_GATEWAY_COMMAND_UUID) {
                slot->command_val_handle = characteristic->val_handle;
            } else if (slot->conn_handle == conn_handle &&
                       uuid == BLE_GATEWAY_STATUS_UUID) {
                slot->status_val_handle = characteristic->val_handle;
            }
            ble_central_unlock_connections();
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ble_central_abort_discovery(slot, "characteristic discovery error");
        return 0;
    }
    if (slot->command_val_handle == 0 || slot->status_val_handle == 0) {
        ble_central_abort_discovery(slot, "required characteristics not found");
        return 0;
    }

    int rc = ble_gattc_disc_all_dscs(conn_handle, slot->status_val_handle,
                                     slot->service_end_handle,
                                     on_descriptor_discovered, slot);
    if (rc != 0) {
        ble_central_abort_discovery(slot, "failed to start descriptor discovery");
    }
    return 0;
}

static int on_service_discovered(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service,
                                 void *arg)
{
    ble_conn_slot_t *slot = arg;
    if (slot == NULL || slot->conn_handle != conn_handle) return 0;

    if (error->status == 0 && service != NULL) {
        if (ble_central_lock_connections()) {
            if (slot->conn_handle == conn_handle) {
                slot->service_start_handle = service->start_handle;
                slot->service_end_handle = service->end_handle;
            }
            ble_central_unlock_connections();
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ble_central_abort_discovery(slot, "service discovery error");
        return 0;
    }
    if (slot->service_end_handle == 0) {
        ble_central_abort_discovery(slot, "gateway service not found");
        return 0;
    }

    int rc = ble_gattc_disc_all_chrs(conn_handle, slot->service_start_handle,
                                     slot->service_end_handle,
                                     on_characteristic_discovered, slot);
    if (rc != 0) {
        ble_central_abort_discovery(
            slot, "failed to start characteristic discovery");
    }
    return 0;
}
