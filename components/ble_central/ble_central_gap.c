#include <string.h>

#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"

#include "ble_central_internal.h"

static const char *TAG = "ble_central_gap";

static bool gap_ctx_snapshot(const ble_callback_ctx_t *ctx,
                             ble_conn_slot_t *snap, uint16_t conn_handle)
{
    if (!ble_state_ctx_snapshot(ctx, snap)) {
        ble_central_metrics_stale_callback();
        return false;
    }
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        snap->conn_handle != conn_handle) {
        ble_central_metrics_stale_callback();
        return false;
    }
    return true;
}

static void gap_release_connection(ble_conn_ref_t ref, uint16_t conn_handle,
                                   int reason, int64_t now_ms)
{
    char device_id[GW_MSG_DEVICE_ID_LEN];
    bool removing = false;
    ble_addr_t peer_addr;
    bool has_peer_addr = false;

    bool consumed = ble_state_on_disconnect(
        (ble_conn_event_ref_t){.ref = ref, .conn_handle = conn_handle},
        now_ms, device_id, sizeof(device_id), &removing, &peer_addr,
        &has_peer_addr);
    if (!consumed) {
        ESP_LOGD(TAG, "stale disconnect ref slot=%u gen=%u", ref.slot_index,
                 (unsigned)ref.generation);
        return;
    }

    // Runtime state is the single source of truth for connectivity; the
    // persistent mirror was removed with the device_store refactor.
    ESP_LOGW(TAG, "[%s][slot=%u][gen=%u][handle=%u] DISCONNECTED reason=%d",
             device_id, ref.slot_index, (unsigned)ref.generation, conn_handle,
             reason);
    ble_central_emit_disconnected(device_id);

    if (!removing) return;

    int idx = ble_runtime_find(device_id);
    if (idx >= 0) ble_runtime_finalize_remove(idx);

    if (!has_peer_addr) return;
    if (!ble_host_is_ready()) {
        ESP_LOGW(TAG, "[%s] BLE host not synced; bond not deleted", device_id);
        return;
    }
    int rc = ble_store_util_delete_peer(&peer_addr);
    if (rc != 0 && rc != BLE_HS_ENOENT) {
        ESP_LOGE(TAG, "[%s] Could not delete bond: %d", device_id, rc);
    }
}

int ble_central_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    ble_callback_ctx_t *ctx = arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        ble_conn_slot_t snap;
        if (!gap_ctx_snapshot(ctx, &snap, BLE_HS_CONN_HANDLE_NONE)) {
            if (event->connect.status == 0 &&
                event->connect.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(event->connect.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
            }
            return 0;
        }

        char device_id[GW_MSG_DEVICE_ID_LEN];
        if (!ble_runtime_get_device_id(snap.device_index, device_id,
                                       sizeof(device_id))) {
            device_id[0] = '\0';
        }

        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "[%s][slot=%u][gen=%u] Connect failed, status=%d",
                     device_id, ctx->ref.slot_index,
                     (unsigned)ctx->ref.generation, event->connect.status);
            gap_release_connection(ctx->ref, ctx->conn_handle,
                                   event->connect.status, ble_now_ms());
            ble_state_ctx_release_by_ref(ctx->ref);
            return 0;
        }

        if (!ble_state_on_connect_success(ctx->ref,
                                          event->connect.conn_handle,
                                          ble_now_ms())) {
            ble_central_metrics_stale_callback();
            ble_gap_terminate(event->connect.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        struct ble_gap_conn_desc description;
        if (ble_gap_conn_find(event->connect.conn_handle, &description) == 0) {
            ble_runtime_set_peer_addr(snap.device_index,
                                      &description.peer_id_addr);
            // Deferred persistence: never write NVS from a host callback.
            ble_central_identity_submit(device_id, &description.peer_id_addr);
        }

        ble_gattc_exchange_mtu(event->connect.conn_handle, NULL, NULL);
        int security_rc =
            ble_gap_security_initiate(event->connect.conn_handle);
        if (security_rc != 0 && security_rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "[%s] Failed to start link security: %d", device_id,
                     security_rc);
            ble_central_metrics_security_failure();
            ble_gap_terminate(event->connect.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ESP_LOGI(TAG, "[%s][slot=%u][gen=%u][handle=%u] SECURING", device_id,
                 ctx->ref.slot_index, (unsigned)ctx->ref.generation,
                 event->connect.conn_handle);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        ble_conn_slot_t snap;
        if (ble_state_ctx_snapshot(ctx, &snap)) {
            gap_release_connection(ctx->ref, event->disconnect.conn.conn_handle,
                                   event->disconnect.reason, ble_now_ms());
        } else {
            ble_central_metrics_stale_callback();
        }
        ble_state_ctx_release_by_ref(ctx->ref);
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        ble_conn_slot_t snap;
        if (!gap_ctx_snapshot(ctx, &snap, event->notify_rx.conn_handle) ||
            event->notify_rx.attr_handle != snap.status_val_handle) {
            return 0;
        }

        uint16_t packet_len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (packet_len == 0 || packet_len > GW_MSG_MAX_LEN) {
            ESP_LOGE(TAG, "Invalid notify length: %u", packet_len);
            return 0;
        }

        uint8_t buffer[GW_MSG_MAX_LEN];
        uint16_t copied = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buffer, sizeof(buffer),
                                &copied) != 0 ||
            copied != packet_len) {
            ESP_LOGE(TAG, "Failed to copy notify payload");
            return 0;
        }

        char device_id[GW_MSG_DEVICE_ID_LEN];
        if (!ble_runtime_get_device_id(snap.device_index, device_id,
                                       sizeof(device_id))) {
            return 0;
        }
        ble_central_notify_enqueue(device_id, buffer, copied);
        return 0;
    }

    case BLE_GAP_EVENT_MTU: {
        ble_conn_slot_t snap;
        if (!gap_ctx_snapshot(ctx, &snap, event->mtu.conn_handle)) return 0;

        ble_state_update_mtu((ble_conn_event_ref_t){.ref = ctx->ref,
                                                    .conn_handle =
                                                        event->mtu.conn_handle},
                             event->mtu.value);
        char device_id[GW_MSG_DEVICE_ID_LEN];
        if (ble_runtime_get_device_id(snap.device_index, device_id,
                                      sizeof(device_id))) {
            ESP_LOGI(TAG, "[%s][handle=%u] MTU updated to %u", device_id,
                     event->mtu.conn_handle, event->mtu.value);
        }
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
        ble_conn_slot_t snap;
        if (!gap_ctx_snapshot(ctx, &snap, event->enc_change.conn_handle)) {
            return 0;
        }

        if (event->enc_change.status != 0) {
            char device_id[GW_MSG_DEVICE_ID_LEN];
            ble_runtime_get_device_id(snap.device_index, device_id,
                                      sizeof(device_id));
            ESP_LOGW(TAG, "[%s] Security setup failed: %d", device_id,
                     event->enc_change.status);
            ble_central_metrics_security_failure();
            ble_gap_terminate(event->enc_change.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        char device_id[GW_MSG_DEVICE_ID_LEN];
        ble_runtime_get_device_id(snap.device_index, device_id,
                                  sizeof(device_id));
        ESP_LOGI(TAG, "[%s][slot=%u][gen=%u][handle=%u] DISCOVERING", device_id,
                 ctx->ref.slot_index, (unsigned)ctx->ref.generation,
                 event->enc_change.conn_handle);
        ble_central_start_gatt_discovery(ctx);
        return 0;
    }

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
