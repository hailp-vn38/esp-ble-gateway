#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"

#include "ble_central_internal.h"
#include "cbor_codec.h"
#include "device_store.h"

static const char *TAG = "ble_central_gap";

int ble_central_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    ble_conn_slot_t *slot = arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (slot == NULL) return 0;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "[%s] Connect failed, status=%d", slot->device_id,
                     event->connect.status);
            if (ble_central_lock_connections()) {
                ble_central_reset_runtime_handles(slot);
                if (slot->forget_requested) {
                    memset(slot, 0, sizeof(*slot));
                } else {
                    slot->state = BLE_CONN_SLOT_IDLE;
                }
                ble_central_unlock_connections();
            }
            return 0;
        }

        if (ble_central_lock_connections()) {
            slot->conn_handle = event->connect.conn_handle;
            slot->state = BLE_CONN_SLOT_SECURING;
            slot->discovery_started_ms = esp_timer_get_time() / 1000;
            ble_central_unlock_connections();
        }
        device_store_set_connected(slot->device_id, 0);

        struct ble_gap_conn_desc description;
        if (ble_gap_conn_find(slot->conn_handle, &description) == 0) {
            device_store_set_ble_addr(slot->device_id,
                                      description.peer_id_addr.val,
                                      description.peer_id_addr.type);
            if (ble_central_lock_connections()) {
                slot->peer_addr = description.peer_id_addr;
                ble_central_unlock_connections();
            }
        }

        ble_gattc_exchange_mtu(slot->conn_handle, NULL, NULL);
        int security_rc = ble_gap_security_initiate(slot->conn_handle);
        if (security_rc != 0 && security_rc != BLE_HS_EALREADY) {
            ble_central_abort_discovery(slot, "failed to start link security");
            return 0;
        }
        ESP_LOGI(TAG, "[%s] Link connected, securing", slot->device_id);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (slot == NULL) return 0;
        ESP_LOGW(TAG, "[%s] Disconnected, reason=%d", slot->device_id,
                 event->disconnect.reason);
        device_store_set_connected(slot->device_id, 0);
        if (ble_central_lock_connections()) {
            ble_central_reset_runtime_handles(slot);
            slot->last_attempt_ms = esp_timer_get_time() / 1000;
            if (slot->forget_requested) {
                memset(slot, 0, sizeof(*slot));
            } else {
                slot->state = BLE_CONN_SLOT_IDLE;
            }
            ble_central_unlock_connections();
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        ble_central_notify_cb_t notify_cb = ble_central_notify_callback();
        if (slot == NULL || notify_cb == NULL ||
            event->notify_rx.attr_handle != slot->status_val_handle) {
            return 0;
        }
        uint16_t packet_len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (packet_len == 0 || packet_len > GW_MSG_MAX_LEN) {
            ESP_LOGE(TAG, "[%s] Invalid notify length: %u", slot->device_id,
                     packet_len);
            return 0;
        }

        uint8_t buffer[GW_MSG_MAX_LEN];
        uint16_t copied = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buffer, sizeof(buffer),
                                &copied) != 0 ||
            copied != packet_len) {
            ESP_LOGE(TAG, "[%s] Failed to copy notify payload", slot->device_id);
            return 0;
        }

        gw_message_t message;
        if (cbor_codec_decode(buffer, copied, &message) == 0) {
            notify_cb(slot->device_id, &message);
        } else {
            ESP_LOGE(TAG, "[%s] Invalid CBOR notify", slot->device_id);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        if (slot != NULL) {
            ESP_LOGI(TAG, "[%s] MTU updated to %u", slot->device_id,
                     event->mtu.value);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (slot == NULL) return 0;
        if (event->enc_change.status != 0) {
            ESP_LOGW(TAG, "[%s] Security setup failed: %d", slot->device_id,
                     event->enc_change.status);
            ble_central_abort_discovery(slot, "link security failed");
        } else {
            ESP_LOGI(TAG, "[%s] Link secured, discovering GATT", slot->device_id);
            ble_central_start_gatt_discovery(slot);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc repeat_description;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle,
                              &repeat_description) == 0) {
            ble_store_util_delete_peer(&repeat_description.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}
