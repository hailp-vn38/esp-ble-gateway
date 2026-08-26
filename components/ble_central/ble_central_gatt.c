#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

#include "ble_central_internal.h"

static const char *TAG = "ble_central_gatt";

static int on_service_discovered(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service,
                                 void *arg);

void ble_central_abort_discovery(ble_callback_ctx_t *ctx, const char *reason)
{
    ble_conn_slot_t snap;
    if (!ble_state_ctx_snapshot(ctx, &snap)) {
        ble_central_metrics_stale_callback();
        return;
    }

    char device_id[GW_MSG_DEVICE_ID_LEN];
    if (!ble_runtime_get_device_id(snap.device_index, device_id,
                                   sizeof(device_id))) {
        device_id[0] = '\0';
    }

    ESP_LOGE(TAG, "[%s][slot=%u][gen=%u][handle=%u] GATT discovery failed: %s",
             device_id, ctx->ref.slot_index, (unsigned)ctx->ref.generation,
             snap.conn_handle, reason);
    ble_central_metrics_discovery_failure();

    if (snap.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(snap.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void ble_central_start_gatt_discovery(ble_callback_ctx_t *ctx)
{
    uint16_t conn_handle =
        ble_state_begin_discovery(ctx->ref, ble_now_ms());
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    int rc = ble_gattc_disc_svc_by_uuid(conn_handle,
                                        &g_ble_gateway_service_uuid.u,
                                        on_service_discovered, ctx);
    if (rc != 0) {
        ble_central_abort_discovery(ctx, "failed to start service discovery");
    }
}

static void gatt_mark_ready(ble_callback_ctx_t *ctx, const ble_conn_slot_t *snap)
{
    char device_id[GW_MSG_DEVICE_ID_LEN];
    if (!ble_runtime_get_device_id(snap->device_index, device_id,
                                   sizeof(device_id))) {
        device_id[0] = '\0';
    }

    ble_state_set_ready(ctx->ref);
    ble_scheduler_note_success(snap->device_index);
    ESP_LOGI(TAG,
             "[%s][slot=%u][gen=%u][handle=%u] READY (command=%u status=%u "
             "cccd=%u mtu=%u)",
             device_id, ctx->ref.slot_index, (unsigned)ctx->ref.generation,
             snap->conn_handle, snap->command_val_handle,
             snap->status_val_handle, snap->status_cccd_handle, snap->mtu);
    ble_central_emit_ready(device_id);
}

static int on_subscribe_write(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    ble_callback_ctx_t *ctx = arg;

    ble_conn_slot_t snap;
    if (!ble_state_ctx_snapshot(ctx, &snap) ||
        snap.conn_handle != conn_handle) {
        ble_central_metrics_stale_callback();
        return 0;
    }

    if (error->status != 0) {
        ble_central_abort_discovery(ctx, "could not write CCCD");
        return 0;
    }

    gatt_mark_ready(ctx, &snap);
    return 0;
}

static int on_descriptor_discovered(uint16_t conn_handle,
                                    const struct ble_gatt_error *error,
                                    uint16_t chr_val_handle,
                                    const struct ble_gatt_dsc *descriptor,
                                    void *arg)
{
    ble_callback_ctx_t *ctx = arg;

    ble_conn_slot_t snap;
    if (!ble_state_ctx_snapshot(ctx, &snap) ||
        snap.conn_handle != conn_handle) {
        ble_central_metrics_stale_callback();
        return 0;
    }

    if (error->status == 0 && descriptor != NULL) {
        if (chr_val_handle == snap.status_val_handle &&
            ble_uuid_cmp(&descriptor->uuid.u,
                         BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) ==
                0) {
            ble_state_set_cccd_handle(ctx->ref, chr_val_handle,
                                      descriptor->handle);
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ble_central_abort_discovery(ctx, "descriptor discovery error");
        return 0;
    }
    if (snap.status_cccd_handle == 0) {
        ble_central_abort_discovery(ctx, "STATUS CCCD not found");
        return 0;
    }

    const uint8_t notify_enable[] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn_handle, snap.status_cccd_handle,
                                  notify_enable, sizeof(notify_enable),
                                  on_subscribe_write, ctx);
    if (rc != 0) {
        ble_central_abort_discovery(ctx, "failed to start CCCD write");
    }
    return 0;
}

static int on_characteristic_discovered(
    uint16_t conn_handle, const struct ble_gatt_error *error,
    const struct ble_gatt_chr *characteristic, void *arg)
{
    ble_callback_ctx_t *ctx = arg;

    ble_conn_slot_t snap;
    if (!ble_state_ctx_snapshot(ctx, &snap) ||
        snap.conn_handle != conn_handle) {
        ble_central_metrics_stale_callback();
        return 0;
    }

    if (error->status == 0 && characteristic != NULL) {
        uint16_t uuid = ble_uuid_u16(&characteristic->uuid.u);
        if (uuid == BLE_GATEWAY_COMMAND_UUID) {
            if (!(characteristic->properties & BLE_GATT_CHR_F_WRITE_NO_RSP)) {
                ble_central_abort_discovery(
                    ctx, "COMMAND characteristic missing WRITE_NO_RSP");
                return 0;
            }
            ble_state_set_char_handle(ctx->ref, uuid,
                                      characteristic->val_handle);
        } else if (uuid == BLE_GATEWAY_STATUS_UUID) {
            if (!(characteristic->properties & BLE_GATT_CHR_F_NOTIFY)) {
                ble_central_abort_discovery(
                    ctx, "STATUS characteristic missing NOTIFY");
                return 0;
            }
            ble_state_set_char_handle(ctx->ref, uuid,
                                      characteristic->val_handle);
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ble_central_abort_discovery(ctx, "characteristic discovery error");
        return 0;
    }

    if (snap.command_val_handle == 0 || snap.status_val_handle == 0) {
        ble_central_abort_discovery(ctx, "required characteristics not found");
        return 0;
    }

    int rc = ble_gattc_disc_all_dscs(conn_handle, snap.status_val_handle,
                                     snap.service_end_handle,
                                     on_descriptor_discovered, ctx);
    if (rc != 0) {
        ble_central_abort_discovery(ctx, "failed to start descriptor discovery");
    }
    return 0;
}

static int on_service_discovered(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service,
                                 void *arg)
{
    ble_callback_ctx_t *ctx = arg;

    ble_conn_slot_t snap;
    if (!ble_state_ctx_snapshot(ctx, &snap) ||
        snap.conn_handle != conn_handle) {
        ble_central_metrics_stale_callback();
        return 0;
    }

    if (error->status == 0 && service != NULL) {
        ble_state_set_service_range(ctx->ref, service->start_handle,
                                    service->end_handle);
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ble_central_abort_discovery(ctx, "service discovery error");
        return 0;
    }
    if (snap.service_end_handle == 0) {
        ble_central_abort_discovery(ctx, "gateway service not found");
        return 0;
    }

    int rc = ble_gattc_disc_all_chrs(conn_handle, snap.service_start_handle,
                                     snap.service_end_handle,
                                     on_characteristic_discovered, ctx);
    if (rc != 0) {
        ble_central_abort_discovery(
            ctx, "failed to start characteristic discovery");
    }
    return 0;
}
