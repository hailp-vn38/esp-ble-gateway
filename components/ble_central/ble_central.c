#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "ble_central.h"
#include "device_store.h"

static const char *TAG = "ble_central";

// UUID phai khop voi thiet bi con (device-module/main/device_config.h)
#define GATT_SVC_UUID           0xABF0
#define GATT_CHR_COMMAND_UUID   0xABF1
#define GATT_CHR_STATUS_UUID    0xABF2

#define CONN_ITVL_UNITS   12   // 12 * 1.25ms = 15ms (dung theo khuyen nghi da chot)
#define CONN_LATENCY       0
#define CONN_TIMEOUT_UNITS 15  // 15 * 10ms = 150ms

typedef struct {
    int      in_use;
    char     device_id[GW_MSG_DEVICE_ID_LEN];
    uint16_t conn_handle;
    uint16_t command_val_handle;
    uint16_t status_val_handle;
    uint16_t status_cccd_handle;
} conn_slot_t;

static conn_slot_t s_conns[BLE_CENTRAL_MAX_CONN];
static ble_central_notify_cb_t s_notify_cb = NULL;

static conn_slot_t *find_slot_by_device_id(const char *device_id)
{
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if (s_conns[i].in_use && strcmp(s_conns[i].device_id, device_id) == 0) return &s_conns[i];
    }
    return NULL;
}

static conn_slot_t *find_slot_by_conn_handle(uint16_t conn_handle)
{
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if (s_conns[i].in_use && s_conns[i].conn_handle == conn_handle) return &s_conns[i];
    }
    return NULL;
}

static conn_slot_t *alloc_slot(void)
{
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if (!s_conns[i].in_use) return &s_conns[i];
    }
    return NULL;
}

static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg)
{
    conn_slot_t *slot = find_slot_by_conn_handle(conn_handle);
    if (slot == NULL || chr == NULL) return 0;

    uint16_t chr_uuid = ble_uuid_u16(&chr->uuid.u);
    if (chr_uuid == GATT_CHR_COMMAND_UUID) {
        slot->command_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "[%s] Found COMMAND char, handle=%d", slot->device_id, chr->val_handle);
    } else if (chr_uuid == GATT_CHR_STATUS_UUID) {
        slot->status_val_handle = chr->val_handle;
        slot->status_cccd_handle = chr->val_handle + 1;
        ESP_LOGI(TAG, "[%s] Found STATUS char, handle=%d", slot->device_id, chr->val_handle);

        uint8_t value[2] = {0x01, 0x00};
        ble_gattc_write_flat(conn_handle, slot->status_cccd_handle, value, sizeof(value), NULL, NULL);
    }
    return 0;
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Connect failed, status=%d", event->connect.status);
            return 0;
        }

        conn_slot_t *slot = NULL;
        for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
            if (s_conns[i].in_use && s_conns[i].conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                slot = &s_conns[i];
                break;
            }
        }
        if (slot == NULL) {
            ESP_LOGE(TAG, "No pending slot found for new connection");
            return 0;
        }

        slot->conn_handle = event->connect.conn_handle;
        device_store_set_connected(slot->device_id, 1);
        ESP_LOGI(TAG, "[%s] Connected, conn_handle=%d", slot->device_id, slot->conn_handle);

        ble_gattc_disc_all_chrs(slot->conn_handle, 0x0001, 0xffff, on_chr_disc, NULL);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        conn_slot_t *slot = find_slot_by_conn_handle(event->disconnect.conn.conn_handle);
        if (slot != NULL) {
            ESP_LOGW(TAG, "[%s] Disconnected, reason=%d", slot->device_id, event->disconnect.reason);
            device_store_set_connected(slot->device_id, 0);
            slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
            slot->command_val_handle = 0;
            slot->status_val_handle = 0;
            // Slot van giu in_use=1 + device_id de co the reconnect sau nay
        }
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        conn_slot_t *slot = find_slot_by_conn_handle(event->notify_rx.conn_handle);
        if (slot == NULL || s_notify_cb == NULL) return 0;

        uint8_t buf[GW_MSG_MAX_LEN];
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > sizeof(buf)) len = sizeof(buf);

        int rc = ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to flatten notify mbuf: %d", rc);
            return 0;
        }

        gw_message_t msg;
        if (cbor_codec_decode(buf, len, &msg) == 0) {
            s_notify_cb(slot->device_id, &msg);
        } else {
            ESP_LOGE(TAG, "[%s] Failed to decode notify payload", slot->device_id);
        }
        return 0;
    }

    default:
        return 0;
    }
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_ble_host_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synced (Central role ready)");
}

int ble_central_init(ble_central_notify_cb_t notify_cb)
{
    memset(s_conns, 0, sizeof(s_conns));
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) s_conns[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_notify_cb = notify_cb;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return -1;
    }

    ble_hs_cfg.sync_cb = on_ble_host_sync;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;

    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "BLE Central initialized (max_conn=%d)", BLE_CENTRAL_MAX_CONN);
    return 0;
}

int ble_central_connect(const char *device_id, const uint8_t *ble_addr, uint8_t addr_type)
{
    if (find_slot_by_device_id(device_id) != NULL) {
        ESP_LOGW(TAG, "[%s] Already has a slot (connected or pending)", device_id);
        return -1;
    }

    conn_slot_t *slot = alloc_slot();
    if (slot == NULL) {
        ESP_LOGE(TAG, "No free connection slot (max=%d)", BLE_CENTRAL_MAX_CONN);
        return -1;
    }

    memset(slot, 0, sizeof(conn_slot_t));
    slot->in_use = 1;
    slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    strncpy(slot->device_id, device_id, sizeof(slot->device_id) - 1);

    ble_addr_t addr;
    addr.type = addr_type;
    memcpy(addr.val, ble_addr, 6);

    struct ble_gap_conn_params conn_params = {0};
    conn_params.scan_itvl = 16;
    conn_params.scan_window = 16;
    conn_params.itvl_min = CONN_ITVL_UNITS;
    conn_params.itvl_max = CONN_ITVL_UNITS;
    conn_params.latency = CONN_LATENCY;
    conn_params.supervision_timeout = CONN_TIMEOUT_UNITS;

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 5000, &conn_params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "[%s] ble_gap_connect failed: %d", device_id, rc);
        slot->in_use = 0;
        return -1;
    }

    ESP_LOGI(TAG, "[%s] Connecting...", device_id);
    return 0;
}

int ble_central_disconnect(const char *device_id)
{
    conn_slot_t *slot = find_slot_by_device_id(device_id);
    if (slot == NULL || slot->conn_handle == BLE_HS_CONN_HANDLE_NONE) return -1;

    int rc = ble_gap_terminate(slot->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return (rc == 0) ? 0 : -1;
}

int ble_central_send_command(const char *device_id, const gw_message_t *msg)
{
    conn_slot_t *slot = find_slot_by_device_id(device_id);
    if (slot == NULL || slot->conn_handle == BLE_HS_CONN_HANDLE_NONE || slot->command_val_handle == 0) {
        ESP_LOGW(TAG, "[%s] Cannot send: not connected or command char not discovered", device_id);
        return -1;
    }

    uint8_t buf[GW_MSG_MAX_LEN];
    int len = cbor_codec_encode(msg, buf, sizeof(buf));
    if (len < 0) {
        ESP_LOGE(TAG, "[%s] Failed to encode command", device_id);
        return -1;
    }

    int rc = ble_gattc_write_no_rsp_flat(slot->conn_handle, slot->command_val_handle, buf, len);
    if (rc != 0) {
        ESP_LOGE(TAG, "[%s] ble_gattc_write_no_rsp_flat failed: %d", device_id, rc);
        return -1;
    }
    return 0;
}

int ble_central_is_connected(const char *device_id)
{
    conn_slot_t *slot = find_slot_by_device_id(device_id);
    return (slot != NULL && slot->conn_handle != BLE_HS_CONN_HANDLE_NONE) ? 1 : 0;
}
