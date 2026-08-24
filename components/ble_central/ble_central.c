#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "ble_central.h"
#include "device_store.h"

/* Provided by NimBLE's store/config component. */
void ble_store_config_init(void);

static const char *TAG = "ble_central";

#define GATT_SVC_UUID             0xABF0
#define GATT_CHR_COMMAND_UUID     0xABF1
#define GATT_CHR_STATUS_UUID      0xABF2

#define CONN_ITVL_FAST_UNITS      12  /* 15 ms */
#define CONN_ITVL_MEDIUM_UNITS    24  /* 30 ms */
#define CONN_ITVL_BUSY_UNITS      40  /* 50 ms */
#define CONN_LATENCY               0
#define CONN_TIMEOUT_UNITS       200  /* 2 seconds; tolerates RF obstruction. */
#define CONNECT_TIMEOUT_MS     10000
#define DISCOVERY_TIMEOUT_MS   10000
#define RECONNECT_INTERVAL_MS   8000
#define SUPERVISOR_TICK_MS      1000

typedef enum {
    CONN_SLOT_FREE = 0,
    CONN_SLOT_IDLE,
    CONN_SLOT_CONNECTING,
    CONN_SLOT_SECURING,
    CONN_SLOT_DISCOVERING,
    CONN_SLOT_READY,
} conn_slot_state_t;

typedef struct {
    conn_slot_state_t state;
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
} conn_slot_t;

static conn_slot_t s_conns[BLE_CENTRAL_MAX_CONN];
static SemaphoreHandle_t s_conn_mutex;
static ble_central_notify_cb_t s_notify_cb;
static ble_central_scan_result_cb_t s_scan_result_cb;
static TaskHandle_t s_reconnect_task;
static volatile bool s_reconnect_running;
static volatile bool s_host_synced;
static volatile bool s_scanning;
static uint8_t s_own_addr_type;
static const ble_uuid16_t s_gateway_service_uuid = BLE_UUID16_INIT(GATT_SVC_UUID);

static bool lock_connections(void)
{
    return s_conn_mutex != NULL &&
           xSemaphoreTake(s_conn_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static void unlock_connections(void)
{
    xSemaphoreGive(s_conn_mutex);
}

static conn_slot_t *find_slot_by_device_id_unlocked(const char *device_id)
{
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if (s_conns[i].state != CONN_SLOT_FREE &&
            strcmp(s_conns[i].device_id, device_id) == 0) {
            return &s_conns[i];
        }
    }
    return NULL;
}

static conn_slot_t *allocate_slot_unlocked(void)
{
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if (s_conns[i].state == CONN_SLOT_FREE) return &s_conns[i];
    }
    return NULL;
}

static void reset_runtime_handles(conn_slot_t *slot)
{
    slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    slot->service_start_handle = 0;
    slot->service_end_handle = 0;
    slot->command_val_handle = 0;
    slot->status_val_handle = 0;
    slot->status_cccd_handle = 0;
    slot->discovery_started_ms = 0;
}

static int active_connection_count_unlocked(void)
{
    int count = 0;
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if (s_conns[i].conn_handle != BLE_HS_CONN_HANDLE_NONE) count++;
    }
    return count;
}

static uint16_t calculate_conn_interval(void)
{
    int active = 0;
    if (lock_connections()) {
        active = active_connection_count_unlocked();
        unlock_connections();
    }
    if (active < 3) return CONN_ITVL_FAST_UNITS;
    if (active < 6) return CONN_ITVL_MEDIUM_UNITS;
    return CONN_ITVL_BUSY_UNITS;
}

static void abort_discovery(conn_slot_t *slot, const char *reason)
{
    ESP_LOGE(TAG, "[%s] GATT discovery failed: %s", slot->device_id, reason);
    if (slot->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(slot->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int on_service_discovered(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service,
                                 void *arg);

static void start_gatt_discovery(conn_slot_t *slot)
{
    if (slot == NULL) return;
    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    if (lock_connections()) {
        if (slot->state == CONN_SLOT_SECURING) {
            slot->state = CONN_SLOT_DISCOVERING;
            slot->discovery_started_ms = esp_timer_get_time() / 1000;
            conn_handle = slot->conn_handle;
        }
        unlock_connections();
    }
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &s_gateway_service_uuid.u,
                                        on_service_discovered, slot);
    if (rc != 0) abort_discovery(slot, "failed to start service discovery");
}

static int on_subscribe_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    conn_slot_t *slot = arg;
    if (slot == NULL || slot->conn_handle != conn_handle) return 0;
    if (error->status != 0) {
        abort_discovery(slot, "could not write CCCD");
        return 0;
    }

    bool ready = false;
    if (lock_connections()) {
        if (slot->conn_handle == conn_handle) slot->state = CONN_SLOT_READY;
        ready = slot->conn_handle == conn_handle;
        unlock_connections();
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
    conn_slot_t *slot = arg;
    if (slot == NULL || slot->conn_handle != conn_handle) return 0;

    if (error->status == 0 && descriptor != NULL) {
        if (chr_val_handle == slot->status_val_handle &&
            ble_uuid_cmp(&descriptor->uuid.u,
                         BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            if (lock_connections()) {
                if (slot->conn_handle == conn_handle) {
                    slot->status_cccd_handle = descriptor->handle;
                }
                unlock_connections();
            }
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        abort_discovery(slot, "descriptor discovery error");
        return 0;
    }
    if (slot->status_cccd_handle == 0) {
        abort_discovery(slot, "STATUS CCCD not found");
        return 0;
    }

    const uint8_t notify_enable[] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn_handle, slot->status_cccd_handle,
                                  notify_enable, sizeof(notify_enable),
                                  on_subscribe_write, slot);
    if (rc != 0) abort_discovery(slot, "failed to start CCCD write");
    return 0;
}

static int on_characteristic_discovered(uint16_t conn_handle,
                                        const struct ble_gatt_error *error,
                                        const struct ble_gatt_chr *characteristic,
                                        void *arg)
{
    conn_slot_t *slot = arg;
    if (slot == NULL || slot->conn_handle != conn_handle) return 0;

    if (error->status == 0 && characteristic != NULL) {
        uint16_t uuid = ble_uuid_u16(&characteristic->uuid.u);
        if (lock_connections()) {
            if (slot->conn_handle == conn_handle && uuid == GATT_CHR_COMMAND_UUID) {
                slot->command_val_handle = characteristic->val_handle;
            } else if (slot->conn_handle == conn_handle &&
                       uuid == GATT_CHR_STATUS_UUID) {
                slot->status_val_handle = characteristic->val_handle;
            }
            unlock_connections();
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        abort_discovery(slot, "characteristic discovery error");
        return 0;
    }
    if (slot->command_val_handle == 0 || slot->status_val_handle == 0) {
        abort_discovery(slot, "required characteristics not found");
        return 0;
    }

    int rc = ble_gattc_disc_all_dscs(conn_handle, slot->status_val_handle,
                                     slot->service_end_handle,
                                     on_descriptor_discovered, slot);
    if (rc != 0) abort_discovery(slot, "failed to start descriptor discovery");
    return 0;
}

static int on_service_discovered(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service,
                                 void *arg)
{
    conn_slot_t *slot = arg;
    if (slot == NULL || slot->conn_handle != conn_handle) return 0;

    if (error->status == 0 && service != NULL) {
        if (lock_connections()) {
            if (slot->conn_handle == conn_handle) {
                slot->service_start_handle = service->start_handle;
                slot->service_end_handle = service->end_handle;
            }
            unlock_connections();
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        abort_discovery(slot, "service discovery error");
        return 0;
    }
    if (slot->service_end_handle == 0) {
        abort_discovery(slot, "gateway service not found");
        return 0;
    }

    int rc = ble_gattc_disc_all_chrs(conn_handle, slot->service_start_handle,
                                     slot->service_end_handle,
                                     on_characteristic_discovered, slot);
    if (rc != 0) abort_discovery(slot, "failed to start characteristic discovery");
    return 0;
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    conn_slot_t *slot = arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (slot == NULL) return 0;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "[%s] Connect failed, status=%d", slot->device_id,
                     event->connect.status);
            if (lock_connections()) {
                reset_runtime_handles(slot);
                if (slot->forget_requested) {
                    memset(slot, 0, sizeof(*slot));
                } else {
                    slot->state = CONN_SLOT_IDLE;
                }
                unlock_connections();
            }
            return 0;
        }

        if (lock_connections()) {
            slot->conn_handle = event->connect.conn_handle;
            slot->state = CONN_SLOT_SECURING;
            slot->discovery_started_ms = esp_timer_get_time() / 1000;
            unlock_connections();
        }
        device_store_set_connected(slot->device_id, 0);

        struct ble_gap_conn_desc description;
        if (ble_gap_conn_find(slot->conn_handle, &description) == 0) {
            device_store_set_ble_addr(slot->device_id, description.peer_id_addr.val,
                                      description.peer_id_addr.type);
            if (lock_connections()) {
                slot->peer_addr = description.peer_id_addr;
                unlock_connections();
            }
        }

        ble_gattc_exchange_mtu(slot->conn_handle, NULL, NULL);
        int security_rc = ble_gap_security_initiate(slot->conn_handle);
        if (security_rc != 0 && security_rc != BLE_HS_EALREADY) {
            abort_discovery(slot, "failed to start link security");
            return 0;
        }
        ESP_LOGI(TAG, "[%s] Link connected, securing", slot->device_id);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (slot == NULL) return 0;
        ESP_LOGW(TAG, "[%s] Disconnected, reason=%d", slot->device_id,
                 event->disconnect.reason);
        device_store_set_connected(slot->device_id, 0);
        if (lock_connections()) {
            reset_runtime_handles(slot);
            slot->last_attempt_ms = esp_timer_get_time() / 1000;
            if (slot->forget_requested) {
                memset(slot, 0, sizeof(*slot));
            } else {
                slot->state = CONN_SLOT_IDLE;
            }
            unlock_connections();
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (slot == NULL || s_notify_cb == NULL ||
            event->notify_rx.attr_handle != slot->status_val_handle) {
            return 0;
        }
        uint16_t packet_len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (packet_len == 0 || packet_len > GW_MSG_MAX_LEN) {
            ESP_LOGE(TAG, "[%s] Invalid notify length: %u", slot->device_id, packet_len);
            return 0;
        }

        uint8_t buffer[GW_MSG_MAX_LEN];
        uint16_t copied = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buffer, sizeof(buffer), &copied) != 0 ||
            copied != packet_len) {
            ESP_LOGE(TAG, "[%s] Failed to copy notify payload", slot->device_id);
            return 0;
        }

        gw_message_t message;
        if (cbor_codec_decode(buffer, copied, &message) == 0) {
            s_notify_cb(slot->device_id, &message);
        } else {
            ESP_LOGE(TAG, "[%s] Invalid CBOR notify", slot->device_id);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        if (slot != NULL) {
            ESP_LOGI(TAG, "[%s] MTU updated to %u", slot->device_id, event->mtu.value);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (slot == NULL) return 0;
        if (event->enc_change.status != 0) {
            ESP_LOGW(TAG, "[%s] Security setup failed: %d", slot->device_id,
                     event->enc_change.status);
            abort_discovery(slot, "link security failed");
        } else {
            ESP_LOGI(TAG, "[%s] Link secured, discovering GATT", slot->device_id);
            start_gatt_discovery(slot);
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

static bool advertisement_has_gateway_service(const struct ble_hs_adv_fields *fields)
{
    for (int i = 0; i < fields->num_uuids16; i++) {
        if (ble_uuid_cmp(&fields->uuids16[i].u, &s_gateway_service_uuid.u) == 0) {
            return true;
        }
    }
    return false;
}

static int scan_event_handler(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_scanning = false;
        ESP_LOGI(TAG, "BLE scan complete, reason=%d", event->disc_complete.reason);
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                event->disc.length_data) != 0 ||
        !advertisement_has_gateway_service(&fields)) {
        return 0;
    }

    ble_scan_result_t result = {0};
    memcpy(result.addr, event->disc.addr.val, sizeof(result.addr));
    result.addr_type = event->disc.addr.type;
    result.rssi = event->disc.rssi;
    if (fields.name != NULL && fields.name_len > 0) {
        size_t name_len = fields.name_len < sizeof(result.name) - 1
                              ? fields.name_len
                              : sizeof(result.name) - 1;
        memcpy(result.name, fields.name, name_len);
        result.name[name_len] = '\0';
    }
    if (s_scan_result_cb != NULL) s_scan_result_cb(&result);
    return 0;
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_ble_host_reset(int reason)
{
    s_host_synced = false;
    s_scanning = false;
    ESP_LOGE(TAG, "NimBLE host reset, reason=%d", reason);
    if (lock_connections()) {
        for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
            if (s_conns[i].state != CONN_SLOT_FREE) {
                device_store_set_connected(s_conns[i].device_id, 0);
                if (s_conns[i].forget_requested) {
                    memset(&s_conns[i], 0, sizeof(s_conns[i]));
                } else {
                    reset_runtime_handles(&s_conns[i]);
                    s_conns[i].state = CONN_SLOT_IDLE;
                }
            }
        }
        unlock_connections();
    }
}

static void on_ble_host_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not infer own BLE address type: %d", rc);
        return;
    }
    s_host_synced = true;
    ESP_LOGI(TAG, "NimBLE host synced (own_addr_type=%u)", s_own_addr_type);
}

int ble_central_init(ble_central_notify_cb_t notify_cb)
{
    memset(s_conns, 0, sizeof(s_conns));
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) reset_runtime_handles(&s_conns[i]);
    s_notify_cb = notify_cb;
    s_host_synced = false;
    s_scanning = false;

    if (s_conn_mutex == NULL) s_conn_mutex = xSemaphoreCreateMutex();
    if (s_conn_mutex == NULL) return -1;

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

    ESP_LOGI(TAG, "BLE Central initialized (max_conn=%d)", BLE_CENTRAL_MAX_CONN);
    return 0;
}

int ble_central_connect(const char *device_id, const uint8_t *ble_addr,
                        uint8_t addr_type)
{
    if (device_id == NULL || device_id[0] == '\0' || ble_addr == NULL) return -1;
    if (!s_host_synced) return -2;
    if (!lock_connections()) return -1;

    device_entry_t registered_device;
    if (device_store_get(device_id, &registered_device) != 0) {
        unlock_connections();
        return -3;
    }

    conn_slot_t *slot = find_slot_by_device_id_unlocked(device_id);
    if (slot == NULL) {
        slot = allocate_slot_unlocked();
        if (slot == NULL) {
            unlock_connections();
            ESP_LOGE(TAG, "No free connection slot (max=%d)", BLE_CENTRAL_MAX_CONN);
            return -1;
        }
        memset(slot, 0, sizeof(*slot));
        reset_runtime_handles(slot);
        strlcpy(slot->device_id, device_id, sizeof(slot->device_id));
        slot->state = CONN_SLOT_IDLE;
    }
    if (slot->state != CONN_SLOT_IDLE) {
        unlock_connections();
        return -1;
    }

    slot->peer_addr.type = addr_type;
    memcpy(slot->peer_addr.val, ble_addr, sizeof(slot->peer_addr.val));
    slot->forget_requested = false;
    slot->state = CONN_SLOT_CONNECTING;
    slot->last_attempt_ms = esp_timer_get_time() / 1000;
    ble_addr_t peer_addr = slot->peer_addr;
    unlock_connections();

    uint16_t interval = calculate_conn_interval();
    struct ble_gap_conn_params parameters = {
        .scan_itvl = 16,
        .scan_window = 16,
        .itvl_min = interval,
        .itvl_max = interval,
        .latency = CONN_LATENCY,
        .supervision_timeout = CONN_TIMEOUT_UNITS,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    int rc = ble_gap_connect(s_own_addr_type, &peer_addr, CONNECT_TIMEOUT_MS,
                             &parameters, gap_event_handler, slot);
    if (rc != 0) {
        if (lock_connections()) {
            if (slot->state == CONN_SLOT_CONNECTING) slot->state = CONN_SLOT_IDLE;
            unlock_connections();
        }
        ESP_LOGW(TAG, "[%s] ble_gap_connect failed: %d", device_id, rc);
        return -1;
    }

    ESP_LOGI(TAG, "[%s] Connecting (interval=%.1f ms)", device_id, interval * 1.25f);
    return 0;
}

int ble_central_disconnect(const char *device_id)
{
    if (device_id == NULL || !lock_connections()) return -1;
    conn_slot_t *slot = find_slot_by_device_id_unlocked(device_id);
    uint16_t handle = slot != NULL ? slot->conn_handle : BLE_HS_CONN_HANDLE_NONE;
    unlock_connections();
    if (handle == BLE_HS_CONN_HANDLE_NONE) return -1;
    return ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM) == 0 ? 0 : -1;
}

int ble_central_forget_device(const char *device_id)
{
    if (device_id == NULL || !lock_connections()) return -1;
    conn_slot_t *slot = find_slot_by_device_id_unlocked(device_id);
    ble_addr_t peer_address = {0};
    bool has_peer_address = false;
    if (slot == NULL) {
        unlock_connections();
        device_entry_t entry;
        if (device_store_get(device_id, &entry) == 0 && entry.has_ble_addr) {
            peer_address.type = entry.ble_addr_type;
            memcpy(peer_address.val, entry.ble_addr, sizeof(peer_address.val));
            has_peer_address = true;
        }
    } else {
        peer_address = slot->peer_addr;
        has_peer_address = true;
    }

    conn_slot_state_t state = CONN_SLOT_FREE;
    uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
    if (slot != NULL) {
        state = slot->state;
        handle = slot->conn_handle;
        if (state == CONN_SLOT_IDLE) {
            memset(slot, 0, sizeof(*slot));
        } else {
            slot->forget_requested = true;
        }
        unlock_connections();
    }

    if (state == CONN_SLOT_CONNECTING) {
        ble_gap_conn_cancel();
    } else if (handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (has_peer_address && s_host_synced) {
        int rc = ble_store_util_delete_peer(&peer_address);
        if (rc != 0 && rc != BLE_HS_ENOENT) {
            ESP_LOGW(TAG, "[%s] Could not delete bond: %d", device_id, rc);
        }
    }
    return 0;
}

int ble_central_send_command(const char *device_id, const gw_message_t *msg)
{
    if (device_id == NULL || msg == NULL || !lock_connections()) return -1;
    conn_slot_t *slot = find_slot_by_device_id_unlocked(device_id);
    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    uint16_t value_handle = 0;
    if (slot != NULL && slot->state == CONN_SLOT_READY) {
        conn_handle = slot->conn_handle;
        value_handle = slot->command_val_handle;
    }
    unlock_connections();
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || value_handle == 0) return -1;

    uint8_t buffer[GW_MSG_MAX_LEN];
    int length = cbor_codec_encode(msg, buffer, sizeof(buffer));
    if (length <= 0) return -1;
    int rc = ble_gattc_write_no_rsp_flat(conn_handle, value_handle, buffer, length);
    if (rc != 0) {
        ESP_LOGE(TAG, "[%s] GATT write failed: %d", device_id, rc);
        return -1;
    }
    return 0;
}

int ble_central_is_connected(const char *device_id)
{
    if (device_id == NULL || !lock_connections()) return 0;
    conn_slot_t *slot = find_slot_by_device_id_unlocked(device_id);
    int connected = slot != NULL && slot->state == CONN_SLOT_READY;
    unlock_connections();
    return connected;
}

int ble_central_active_count(void)
{
    if (!lock_connections()) return 0;
    int count = active_connection_count_unlocked();
    unlock_connections();
    return count;
}

int ble_central_scan_start(ble_central_scan_result_cb_t scan_result_cb)
{
    if (!s_host_synced || scan_result_cb == NULL || ble_gap_disc_active()) return -1;
    s_scan_result_cb = scan_result_cb;

    struct ble_gap_disc_params parameters = {
        .itvl = 48,
        .window = 48,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 1,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &parameters,
                          scan_event_handler, NULL);
    if (rc != 0) return -1;
    s_scanning = true;
    ESP_LOGI(TAG, "BLE scan started (service 0x%04X)", GATT_SVC_UUID);
    return 0;
}

int ble_central_scan_stop(void)
{
    if (!ble_gap_disc_active()) {
        s_scanning = false;
        return 0;
    }
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) return -1;
    return 0;
}

int ble_central_is_scanning(void)
{
    return s_scanning && ble_gap_disc_active();
}

static bool snapshot_slot_state(const char *device_id, conn_slot_state_t *state,
                                int64_t *last_attempt_ms)
{
    bool found = false;
    if (lock_connections()) {
        conn_slot_t *slot = find_slot_by_device_id_unlocked(device_id);
        if (slot != NULL) {
            *state = slot->state;
            *last_attempt_ms = slot->last_attempt_ms;
            found = true;
        }
        unlock_connections();
    }
    return found;
}

static void terminate_timed_out_discoveries(int64_t now_ms)
{
    uint16_t timed_out_handles[BLE_CENTRAL_MAX_CONN];
    int timed_out_count = 0;
    if (!lock_connections()) return;
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if ((s_conns[i].state == CONN_SLOT_SECURING ||
             s_conns[i].state == CONN_SLOT_DISCOVERING) &&
            now_ms - s_conns[i].discovery_started_ms >= DISCOVERY_TIMEOUT_MS) {
            timed_out_handles[timed_out_count++] = s_conns[i].conn_handle;
            ESP_LOGE(TAG, "[%s] GATT discovery timed out", s_conns[i].device_id);
        }
    }
    unlock_connections();
    for (int i = 0; i < timed_out_count; i++) {
        ble_gap_terminate(timed_out_handles[i], BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void reconnect_supervisor_task(void *arg)
{
    while (s_reconnect_running) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        terminate_timed_out_discoveries(now_ms);

        if (s_host_synced && !ble_gap_disc_active()) {
            device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
            int count = device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES);
            for (int i = 0; i < count; i++) {
                if (devices[i].connected || !devices[i].has_ble_addr) continue;

                conn_slot_state_t state = CONN_SLOT_IDLE;
                int64_t last_attempt_ms = 0;
                bool has_slot = snapshot_slot_state(devices[i].device_id, &state,
                                                    &last_attempt_ms);
                if (has_slot && state != CONN_SLOT_IDLE) continue;
                if (has_slot && now_ms - last_attempt_ms < RECONNECT_INTERVAL_MS) continue;

                ble_central_connect(devices[i].device_id, devices[i].ble_addr,
                                    devices[i].ble_addr_type);
                /* NimBLE controllers commonly serialize connection procedures. */
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_TICK_MS));
    }

    s_reconnect_task = NULL;
    vTaskDelete(NULL);
}

int ble_central_start_reconnect_supervisor(void)
{
    if (s_reconnect_running) return 0;
    s_reconnect_running = true;
    BaseType_t result = xTaskCreate(reconnect_supervisor_task, "ble_reconnect", 4096,
                                    NULL, 4, &s_reconnect_task);
    if (result != pdPASS) {
        s_reconnect_running = false;
        s_reconnect_task = NULL;
        return -1;
    }
    return 0;
}

void ble_central_stop_reconnect_supervisor(void)
{
    s_reconnect_running = false;
}
