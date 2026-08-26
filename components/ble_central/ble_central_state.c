#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"

#include "ble_central_internal.h"

#define BLE_EVENT_HOST_SYNCED BIT0

static const char *TAG = "ble_central_state";

ble_conn_slot_t g_ble_connections[BLE_CENTRAL_MAX_CONN];
uint8_t g_ble_own_addr_type;
const ble_uuid16_t g_ble_gateway_service_uuid =
    BLE_UUID16_INIT(BLE_GATEWAY_SERVICE_UUID);

static SemaphoreHandle_t s_state_mutex;
static EventGroupHandle_t s_ble_events;
static ble_callback_ctx_t s_callback_ctxs[BLE_CENTRAL_MAX_CONN];
static ble_central_metrics_t s_metrics;

int64_t ble_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

bool ble_state_lock(void)
{
    return s_state_mutex != NULL &&
           xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

void ble_state_unlock(void)
{
    xSemaphoreGive(s_state_mutex);
}

bool ble_host_is_ready(void)
{
    return s_ble_events != NULL &&
           (xEventGroupGetBits(s_ble_events) & BLE_EVENT_HOST_SYNCED) != 0;
}

void ble_host_set_ready(bool ready)
{
    if (s_ble_events == NULL) return;
    if (ready) {
        xEventGroupSetBits(s_ble_events, BLE_EVENT_HOST_SYNCED);
    } else {
        xEventGroupClearBits(s_ble_events, BLE_EVENT_HOST_SYNCED);
    }
}

static void ble_conn_reset_to_free_unlocked(int slot_index)
{
    ble_conn_slot_t *slot = &g_ble_connections[slot_index];
    uint32_t generation = slot->generation;

    memset(slot, 0, sizeof(*slot));

    slot->generation = generation;
    slot->state = BLE_CONN_FREE;
    slot->device_index = -1;
    slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    slot->mtu = 23;
}

void ble_state_reset_slot_free_unlocked(int slot_index)
{
    if (slot_index < 0 || slot_index >= BLE_CENTRAL_MAX_CONN) return;
    ble_conn_reset_to_free_unlocked(slot_index);
}

static void ble_conn_prepare_new_lifetime_unlocked(int slot_index,
                                                   int device_index)
{
    ble_conn_slot_t *slot = &g_ble_connections[slot_index];
    uint32_t generation = slot->generation + 1;

    memset(slot, 0, sizeof(*slot));

    slot->generation = generation;
    slot->state = BLE_CONN_CONNECTING;
    slot->device_index = device_index;
    slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    slot->mtu = 23;
}

static bool ble_ref_valid_unlocked(ble_conn_ref_t ref)
{
    return ref.slot_index < BLE_CENTRAL_MAX_CONN &&
           g_ble_connections[ref.slot_index].state != BLE_CONN_FREE &&
           g_ble_connections[ref.slot_index].generation == ref.generation;
}

static void ble_schedule_retry_unlocked(ble_device_runtime_t *dev,
                                        int64_t now_ms)
{
    int64_t delay = ble_backoff_delay_ms(dev->retry_count);
    dev->retry_count++;
    dev->last_attempt_ms = now_ms;
    dev->next_retry_ms = now_ms + delay;
}

int ble_central_state_init(ble_central_notify_cb_t notify_cb)
{
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    if (s_state_mutex == NULL) return -1;

    if (s_ble_events == NULL) {
        s_ble_events = xEventGroupCreate();
    }
    if (s_ble_events == NULL) return -1;

    memset(g_ble_connections, 0, sizeof(g_ble_connections));
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        ble_conn_reset_to_free_unlocked(i);
        s_callback_ctxs[i].in_use = false;
    }
    memset(&s_metrics, 0, sizeof(s_metrics));
    xEventGroupClearBits(s_ble_events, BLE_EVENT_HOST_SYNCED);

    if (ble_central_notify_init(notify_cb) != 0) return -1;
    return 0;
}

const ble_central_metrics_t *ble_central_metrics(void)
{
    return &s_metrics;
}

void ble_central_metrics_connect_failure(void)
{
    if (ble_state_lock()) {
        s_metrics.connect_failures++;
        ble_state_unlock();
    }
}

void ble_central_metrics_stale_callback(void)
{
    if (ble_state_lock()) {
        s_metrics.stale_callbacks++;
        ble_state_unlock();
    }
}

void ble_central_metrics_security_failure(void)
{
    if (ble_state_lock()) {
        s_metrics.security_failures++;
        ble_state_unlock();
    }
}

void ble_central_metrics_discovery_failure(void)
{
    if (ble_state_lock()) {
        s_metrics.discovery_failures++;
        ble_state_unlock();
    }
}

void ble_central_metrics_notify_enqueued(void)
{
    if (ble_state_lock()) {
        s_metrics.notify_enqueued++;
        ble_state_unlock();
    }
}

void ble_central_metrics_notify_dropped(void)
{
    if (ble_state_lock()) {
        s_metrics.notify_dropped++;
        ble_state_unlock();
    }
}

void ble_central_metrics_notify_decode_error(void)
{
    if (ble_state_lock()) {
        s_metrics.notify_decode_errors++;
        ble_state_unlock();
    }
}

void ble_central_metrics_mtu_reject(void)
{
    if (ble_state_lock()) {
        s_metrics.mtu_rejects++;
        ble_state_unlock();
    }
}

void ble_central_metrics_notify_received(void)
{
    if (ble_state_lock()) {
        s_metrics.notify_received++;
        ble_state_unlock();
    }
}

void ble_central_metrics_identity_persist_failure(void)
{
    if (ble_state_lock()) {
        s_metrics.identity_persist_failures++;
        ble_state_unlock();
    }
}

int ble_state_reserve_connection(int device_index, ble_conn_ref_t *out_ref,
                                 int64_t now_ms)
{
    if (out_ref == NULL || device_index < 0 ||
        device_index >= DEVICE_STORE_MAX_DEVICES) {
        return BLE_CENTRAL_ERR_INVALID_ARG;
    }
    if (!ble_state_lock()) return BLE_CENTRAL_ERR_STATE;

    ble_device_runtime_t *dev = &g_ble_devices[device_index];
    int rc;

    do {
        if (!dev->in_use) {
            rc = BLE_CENTRAL_ERR_NOT_FOUND;
            break;
        }
        if (dev->state != BLE_DEVICE_OFFLINE &&
            dev->state != BLE_DEVICE_BACKOFF) {
            rc = BLE_CENTRAL_ERR_BUSY;
            break;
        }
        if (dev->connection_slot != -1) {
            rc = BLE_CENTRAL_ERR_STATE;
            break;
        }
        bool connecting = false;
        for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
            if (g_ble_connections[i].state == BLE_CONN_CONNECTING) {
                connecting = true;
                break;
            }
        }
        if (connecting) {
            rc = BLE_CENTRAL_ERR_CONNECT_IN_PROGRESS;
            break;
        }

        int slot_index = -1;
        for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
            if (g_ble_connections[i].state == BLE_CONN_FREE) {
                slot_index = i;
                break;
            }
        }
        if (slot_index < 0) {
            rc = BLE_CENTRAL_ERR_NO_SLOT;
            break;
        }

        ble_conn_prepare_new_lifetime_unlocked(slot_index, device_index);
        dev->connection_slot = slot_index;
        dev->state = BLE_DEVICE_CONNECTING;
        dev->last_attempt_ms = now_ms;
        s_metrics.connect_attempts++;

        out_ref->slot_index = (uint8_t)slot_index;
        out_ref->generation = g_ble_connections[slot_index].generation;
        rc = BLE_CENTRAL_OK;
    } while (0);

    ble_state_unlock();
    return rc;
}

void ble_state_rollback_connection_start(ble_conn_ref_t ref, int64_t now_ms)
{
    if (!ble_state_lock()) return;

    if (ref.slot_index < BLE_CENTRAL_MAX_CONN) {
        ble_conn_slot_t *slot = &g_ble_connections[ref.slot_index];
        if (slot->state != BLE_CONN_FREE &&
            slot->generation == ref.generation &&
            slot->device_index >= 0 &&
            slot->device_index < DEVICE_STORE_MAX_DEVICES) {
            ble_conn_reset_to_free_unlocked(ref.slot_index);
        }
    }

    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        ble_device_runtime_t *dev = &g_ble_devices[i];
        if (dev->in_use && dev->connection_slot == ref.slot_index) {
            dev->connection_slot = -1;
            if (dev->state == BLE_DEVICE_CONNECTING) {
                dev->state = BLE_DEVICE_BACKOFF;
                ble_schedule_retry_unlocked(dev, now_ms);
            }
            break;
        }
    }

    s_metrics.connect_failures++;
    ble_state_unlock();
}

bool ble_conn_snapshot(ble_conn_ref_t ref, ble_conn_slot_t *out)
{
    if (out == NULL) return false;
    if (!ble_state_lock()) return false;

    bool ok = ble_ref_valid_unlocked(ref);
    if (ok) {
        *out = g_ble_connections[ref.slot_index];
    }

    ble_state_unlock();
    return ok;
}

int ble_conn_find_by_handle(uint16_t conn_handle, ble_conn_ref_t *out_ref,
                            char *out_device_id, size_t device_id_cap)
{
    if (!ble_state_lock()) return BLE_CENTRAL_ERR_STATE;

    int found = BLE_CENTRAL_ERR_NOT_FOUND;
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        ble_conn_slot_t *slot = &g_ble_connections[i];
        if (slot->state != BLE_CONN_FREE &&
            slot->conn_handle == conn_handle) {
            if (out_ref != NULL) {
                out_ref->slot_index = (uint8_t)i;
                out_ref->generation = slot->generation;
            }
            if (out_device_id != NULL && device_id_cap > 0 &&
                slot->device_index >= 0 &&
                slot->device_index < DEVICE_STORE_MAX_DEVICES) {
                strlcpy(out_device_id,
                        g_ble_devices[slot->device_index].device_id,
                        device_id_cap);
            }
            found = (int)i;
            break;
        }
    }

    ble_state_unlock();
    return found;
}

bool ble_state_on_connect_success(ble_conn_ref_t ref, uint16_t conn_handle,
                                  int64_t now_ms)
{
    if (!ble_state_lock()) return false;

    bool ok = ble_ref_valid_unlocked(ref) &&
              g_ble_connections[ref.slot_index].state == BLE_CONN_CONNECTING;
    if (ok) {
        ble_conn_slot_t *slot = &g_ble_connections[ref.slot_index];
        slot->conn_handle = conn_handle;
        slot->state = BLE_CONN_SECURING;
        slot->started_ms = now_ms;
        for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
            ble_callback_ctx_t *ctx = &s_callback_ctxs[i];
            if (ctx->in_use && ctx->ref.slot_index == ref.slot_index &&
                ctx->ref.generation == ref.generation) {
                ctx->conn_handle = conn_handle;
                break;
            }
        }
    }

    ble_state_unlock();
    return ok;
}

bool ble_state_on_disconnect(ble_conn_event_ref_t event_ref, int64_t now_ms,
                             char *out_device_id, size_t device_id_cap,
                             bool *out_removing, ble_addr_t *out_peer_addr,
                             bool *out_has_peer_addr)
{
    if (!ble_state_lock()) return false;

    ble_conn_slot_t *slot = &g_ble_connections[event_ref.ref.slot_index];
    bool ok = event_ref.ref.slot_index < BLE_CENTRAL_MAX_CONN &&
              slot->state != BLE_CONN_FREE &&
              slot->generation == event_ref.ref.generation &&
              slot->conn_handle == event_ref.conn_handle;
    if (!ok) {
        s_metrics.stale_callbacks++;
        ble_state_unlock();
        return false;
    }

    int device_index = slot->device_index;
    ble_device_runtime_t *dev = NULL;
    if (device_index >= 0 && device_index < DEVICE_STORE_MAX_DEVICES &&
        g_ble_devices[device_index].in_use) {
        dev = &g_ble_devices[device_index];
    }

    if (out_device_id != NULL && device_id_cap > 0) {
        if (dev != NULL) {
            strlcpy(out_device_id, dev->device_id, device_id_cap);
        } else {
            out_device_id[0] = '\0';
        }
    }
    if (out_has_peer_addr != NULL) {
        *out_has_peer_addr = dev != NULL && dev->has_peer_addr;
    }
    if (out_peer_addr != NULL && dev != NULL && dev->has_peer_addr) {
        *out_peer_addr = dev->peer_addr;
    }

    ble_conn_reset_to_free_unlocked(event_ref.ref.slot_index);
    if (dev != NULL) {
        dev->connection_slot = -1;
        if (dev->state != BLE_DEVICE_REMOVING) {
            dev->state = BLE_DEVICE_BACKOFF;
            ble_schedule_retry_unlocked(dev, now_ms);
        }
    }

    s_metrics.disconnects++;
    ble_state_unlock();
    if (out_removing != NULL) {
        *out_removing = dev != NULL && dev->state == BLE_DEVICE_REMOVING;
    }
    return true;
}

void ble_state_mark_disconnecting(ble_conn_ref_t ref)
{
    if (!ble_state_lock()) return;
    if (ble_ref_valid_unlocked(ref) &&
        g_ble_connections[ref.slot_index].state != BLE_CONN_DISCONNECTING) {
        g_ble_connections[ref.slot_index].state = BLE_CONN_DISCONNECTING;
    }
    ble_state_unlock();
}

void ble_state_update_mtu(ble_conn_event_ref_t event_ref, uint16_t mtu)
{
    if (!ble_state_lock()) return;

    ble_conn_slot_t *slot = &g_ble_connections[event_ref.ref.slot_index];
    if (event_ref.ref.slot_index < BLE_CENTRAL_MAX_CONN &&
        slot->state != BLE_CONN_FREE &&
        slot->generation == event_ref.ref.generation &&
        slot->conn_handle == event_ref.conn_handle && mtu >= 23) {
        slot->mtu = mtu;
    } else {
        s_metrics.stale_callbacks++;
    }

    ble_state_unlock();
}

void ble_state_set_ready(ble_conn_ref_t ref)
{
    if (!ble_state_lock()) return;
    if (ble_ref_valid_unlocked(ref)) {
        g_ble_connections[ref.slot_index].state = BLE_CONN_READY;
    }
    ble_state_unlock();
}

uint16_t ble_state_begin_discovery(ble_conn_ref_t ref, int64_t now_ms)
{
    if (!ble_state_lock()) return BLE_HS_CONN_HANDLE_NONE;

    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    if (ble_ref_valid_unlocked(ref) &&
        g_ble_connections[ref.slot_index].state == BLE_CONN_SECURING) {
        g_ble_connections[ref.slot_index].state = BLE_CONN_DISCOVERING;
        g_ble_connections[ref.slot_index].discovery_started_ms = now_ms;
        conn_handle = g_ble_connections[ref.slot_index].conn_handle;
    }

    ble_state_unlock();
    return conn_handle;
}

bool ble_state_set_service_range(ble_conn_ref_t ref, uint16_t start_handle,
                                 uint16_t end_handle)
{
    if (!ble_state_lock()) return false;

    bool ok = ble_ref_valid_unlocked(ref);
    if (ok) {
        ble_conn_slot_t *slot = &g_ble_connections[ref.slot_index];
        slot->service_start_handle = start_handle;
        slot->service_end_handle = end_handle;
    }

    ble_state_unlock();
    return ok;
}

bool ble_state_set_char_handle(ble_conn_ref_t ref, uint16_t uuid16,
                               uint16_t val_handle)
{
    if (!ble_state_lock()) return false;

    bool ok = ble_ref_valid_unlocked(ref);
    if (ok) {
        ble_conn_slot_t *slot = &g_ble_connections[ref.slot_index];
        if (uuid16 == BLE_GATEWAY_COMMAND_UUID) {
            slot->command_val_handle = val_handle;
        } else if (uuid16 == BLE_GATEWAY_STATUS_UUID) {
            slot->status_val_handle = val_handle;
        } else {
            ok = false;
        }
    }

    ble_state_unlock();
    return ok;
}

bool ble_state_set_cccd_handle(ble_conn_ref_t ref, uint16_t chr_val_handle,
                               uint16_t dsc_handle)
{
    if (!ble_state_lock()) return false;

    bool ok = ble_ref_valid_unlocked(ref);
    if (ok) {
        ble_conn_slot_t *slot = &g_ble_connections[ref.slot_index];
        if (slot->status_val_handle == chr_val_handle) {
            slot->status_cccd_handle = dsc_handle;
        } else {
            ok = false;
        }
    }

    ble_state_unlock();
    return ok;
}

size_t ble_state_collect_timeouts(int64_t now_ms, ble_timeout_entry_t *out,
                                  size_t max_entries)
{
    if (out == NULL || max_entries == 0 || !ble_state_lock()) return 0;

    size_t count = 0;
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN && count < max_entries; i++) {
        ble_conn_slot_t *slot = &g_ble_connections[i];
        bool security_timeout =
            slot->state == BLE_CONN_SECURING &&
            now_ms - slot->started_ms >= BLE_SECURITY_TIMEOUT_MS;
        bool discovery_timeout =
            slot->state == BLE_CONN_DISCOVERING &&
            slot->discovery_started_ms > 0 &&
            now_ms - slot->discovery_started_ms >=
                BLE_GATT_DISCOVERY_TIMEOUT_MS;
        if (!security_timeout && !discovery_timeout) continue;

        out[count].conn_handle = slot->conn_handle;
        out[count].is_discovery = discovery_timeout;
        out[count].device_id[0] = '\0';
        if (slot->device_index >= 0 &&
            slot->device_index < DEVICE_STORE_MAX_DEVICES) {
            strlcpy(out[count].device_id,
                    g_ble_devices[slot->device_index].device_id,
                    sizeof(out[count].device_id));
        }
        count++;
    }

    ble_state_unlock();
    return count;
}

size_t ble_state_handle_host_reset(char (*out_device_ids)[GW_MSG_DEVICE_ID_LEN],
                                   size_t max_ids)
{
    if (!ble_state_lock()) return 0;

    size_t mirror_count = 0;
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if (g_ble_connections[i].state == BLE_CONN_FREE) continue;
        g_ble_connections[i].generation++;
        ble_conn_reset_to_free_unlocked(i);
    }
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        s_callback_ctxs[i].in_use = false;
    }

    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        ble_device_runtime_t *dev = &g_ble_devices[i];
        if (!dev->in_use) continue;

        bool had_link = dev->connection_slot != -1 ||
                        dev->state == BLE_DEVICE_CONNECTED ||
                        dev->state == BLE_DEVICE_CONNECTING;
        bool was_removing = dev->state == BLE_DEVICE_REMOVING;

        if ((had_link || was_removing) && mirror_count < max_ids) {
            memcpy(out_device_ids[mirror_count], dev->device_id,
                   GW_MSG_DEVICE_ID_LEN);
            mirror_count++;
        }

        if (was_removing) {
            ESP_LOGW(TAG, "[%s] host reset during removal; bond may be orphaned",
                     dev->device_id);
            memset(dev, 0, sizeof(*dev));
            continue;
        }

        dev->connection_slot = -1;
        if (dev->state == BLE_DEVICE_CONNECTING ||
            dev->state == BLE_DEVICE_CONNECTED ||
            dev->state == BLE_DEVICE_BACKOFF) {
            dev->state = BLE_DEVICE_OFFLINE;
        }
        dev->last_attempt_ms = 0;
        dev->next_retry_ms = 0;
    }

    s_metrics.host_resets++;
    ble_state_unlock();
    return mirror_count;
}

ble_callback_ctx_t *ble_state_ctx_acquire(ble_conn_ref_t ref)
{
    if (!ble_state_lock()) return NULL;

    ble_callback_ctx_t *found = NULL;
    if (ble_ref_valid_unlocked(ref)) {
        for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
            ble_callback_ctx_t *ctx = &s_callback_ctxs[i];
            if (!ctx->in_use) {
                ctx->in_use = true;
                ctx->ref = ref;
                ctx->conn_handle = g_ble_connections[ref.slot_index].conn_handle;
                found = ctx;
                break;
            }
        }
    }

    ble_state_unlock();
    return found;
}

void ble_state_ctx_release_by_ref(ble_conn_ref_t ref)
{
    if (!ble_state_lock()) return;

    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        ble_callback_ctx_t *ctx = &s_callback_ctxs[i];
        if (ctx->in_use && ctx->ref.slot_index == ref.slot_index &&
            ctx->ref.generation == ref.generation) {
            ctx->in_use = false;
            break;
        }
    }

    ble_state_unlock();
}

bool ble_state_ctx_snapshot(const ble_callback_ctx_t *ctx, ble_conn_slot_t *out)
{
    if (ctx == NULL || out == NULL || !ble_state_lock()) return false;

    bool ok = ctx->in_use &&
              ctx->ref.slot_index < BLE_CENTRAL_MAX_CONN &&
              g_ble_connections[ctx->ref.slot_index].state != BLE_CONN_FREE &&
              g_ble_connections[ctx->ref.slot_index].generation ==
                  ctx->ref.generation;
    if (ok) {
        *out = g_ble_connections[ctx->ref.slot_index];
    }

    ble_state_unlock();
    return ok;
}

void ble_state_ctx_update_handle(ble_conn_ref_t ref, uint16_t conn_handle)
{
    if (!ble_state_lock()) return;

    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        ble_callback_ctx_t *ctx = &s_callback_ctxs[i];
        if (ctx->in_use && ctx->ref.slot_index == ref.slot_index &&
            ctx->ref.generation == ref.generation) {
            ctx->conn_handle = conn_handle;
            break;
        }
    }

    ble_state_unlock();
}

int ble_central_active_count_unlocked(void)
{
    int count = 0;
    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if (g_ble_connections[i].state != BLE_CONN_FREE &&
            g_ble_connections[i].conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            count++;
        }
    }
    return count;
}

uint16_t ble_central_calculate_conn_interval(void)
{
    int active = 0;
    if (ble_state_lock()) {
        active = ble_central_active_count_unlocked();
        ble_state_unlock();
    }
    if (active < 3) return BLE_CONN_ITVL_FAST_UNITS;
    if (active < 6) return BLE_CONN_ITVL_MEDIUM_UNITS;
    return BLE_CONN_ITVL_BUSY_UNITS;
}

int64_t ble_backoff_delay_ms(uint8_t retry_count)
{
    int64_t delay = BLE_RETRY_INITIAL_MS;
    for (uint8_t i = 0; i < retry_count && delay < BLE_RETRY_MAX_MS; i++) {
        delay *= 2;
    }
    return delay > BLE_RETRY_MAX_MS ? BLE_RETRY_MAX_MS : delay;
}

int ble_connection_start(int device_index)
{
    ble_conn_ref_t ref;
    int64_t now_ms = ble_now_ms();

    int rc = ble_state_reserve_connection(device_index, &ref, now_ms);
    if (rc != BLE_CENTRAL_OK) return rc;

    char device_id[GW_MSG_DEVICE_ID_LEN];
    ble_addr_t peer_addr;
    if (!ble_runtime_get_device_id(device_index, device_id,
                                   sizeof(device_id)) ||
        !ble_runtime_get_peer_addr(device_index, &peer_addr)) {
        ble_state_rollback_connection_start(ref, ble_now_ms());
        return BLE_CENTRAL_ERR_NOT_FOUND;
    }

    ble_callback_ctx_t *ctx = ble_state_ctx_acquire(ref);
    if (ctx == NULL) {
        ble_state_rollback_connection_start(ref, ble_now_ms());
        return BLE_CENTRAL_ERR_NO_RESOURCE;
    }

    struct ble_gap_conn_params parameters = {
        .scan_itvl = 16,
        .scan_window = 16,
        .itvl_min = ble_central_calculate_conn_interval(),
        .itvl_max = ble_central_calculate_conn_interval(),
        .latency = BLE_CONN_LATENCY,
        .supervision_timeout = BLE_CONN_TIMEOUT_UNITS,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    rc = ble_gap_connect(g_ble_own_addr_type, &peer_addr,
                         BLE_CONNECT_TIMEOUT_MS, &parameters,
                         ble_central_gap_event_handler, ctx);
    if (rc != 0) {
        ble_state_ctx_release_by_ref(ref);
        ble_state_rollback_connection_start(ref, ble_now_ms());
        ESP_LOGW(TAG, "[%s] ble_gap_connect failed: %d", device_id, rc);
        return BLE_CENTRAL_ERR_STACK;
    }

    ESP_LOGI(TAG, "[%s][slot=%u][gen=%u] CONNECTING", device_id,
             ref.slot_index, (unsigned)ref.generation);
    return BLE_CENTRAL_OK;
}
