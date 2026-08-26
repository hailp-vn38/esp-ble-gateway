#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"

#include "ble_central_internal.h"
#include "device_store.h"

ble_device_runtime_t g_ble_devices[DEVICE_STORE_MAX_DEVICES];

static int ble_runtime_find_unlocked(const char *device_id)
{
    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (g_ble_devices[i].in_use &&
            strcmp(g_ble_devices[i].device_id, device_id) == 0) {
            return i;
        }
    }
    return BLE_CENTRAL_ERR_NOT_FOUND;
}

static int ble_runtime_register_unlocked(const char *device_id,
                                         const ble_addr_t *addr)
{
    for (int i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        ble_device_runtime_t *dev = &g_ble_devices[i];
        if (dev->in_use) continue;

        memset(dev, 0, sizeof(*dev));
        dev->in_use = true;
        strlcpy(dev->device_id, device_id, sizeof(dev->device_id));
        if (addr != NULL) {
            dev->peer_addr = *addr;
            dev->has_peer_addr = true;
        }
        dev->connection_slot = -1;
        dev->state = BLE_DEVICE_OFFLINE;
        dev->reconnect_enabled = true;
        dev->retry_count = 0;
        dev->last_attempt_ms = 0;
        dev->next_retry_ms = 0;
        return i;
    }
    return BLE_CENTRAL_ERR_NO_SLOT;
}

int ble_central_runtime_init(void)
{
    device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
    size_t count = 0;
    if (device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES, &count) !=
        DEVICE_STORE_OK) {
        return BLE_CENTRAL_ERR_STACK;
    }

    if (!ble_state_lock()) return BLE_CENTRAL_ERR_STATE;

    memset(g_ble_devices, 0, sizeof(g_ble_devices));
    for (size_t i = 0; i < count; i++) {
        if (!devices[i].has_ble_identity) continue;

        ble_addr_t addr;
        addr.type = devices[i].ble_addr_type;
        memcpy(addr.val, devices[i].ble_addr, sizeof(addr.val));

        int idx = ble_runtime_register_unlocked(devices[i].device_id, &addr);
        if (idx >= 0) {
            ESP_LOGI("ble_central_runtime", "[%s] registered",
                     devices[i].device_id);
        }
    }

    ble_state_unlock();
    return BLE_CENTRAL_OK;
}

int ble_runtime_find(const char *device_id)
{
    if (device_id == NULL || !ble_state_lock()) return BLE_CENTRAL_ERR_INVALID_ARG;

    int idx = ble_runtime_find_unlocked(device_id);

    ble_state_unlock();
    return idx;
}

int ble_runtime_find_or_register(const char *device_id, const ble_addr_t *addr)
{
    if (device_id == NULL || !ble_state_lock()) return BLE_CENTRAL_ERR_INVALID_ARG;

    int idx = ble_runtime_find_unlocked(device_id);
    if (idx >= 0) {
        if (addr != NULL) {
            g_ble_devices[idx].peer_addr = *addr;
            g_ble_devices[idx].has_peer_addr = true;
        }
    } else {
        idx = ble_runtime_register_unlocked(device_id, addr);
    }

    ble_state_unlock();
    return idx;
}

bool ble_runtime_snapshot(int device_index, ble_device_runtime_t *out)
{
    if (out == NULL || device_index < 0 ||
        device_index >= DEVICE_STORE_MAX_DEVICES || !ble_state_lock()) {
        return false;
    }

    bool ok = g_ble_devices[device_index].in_use;
    if (ok) {
        *out = g_ble_devices[device_index];
    }

    ble_state_unlock();
    return ok;
}

bool ble_runtime_get_peer_addr(int device_index, ble_addr_t *out)
{
    if (out == NULL || device_index < 0 ||
        device_index >= DEVICE_STORE_MAX_DEVICES || !ble_state_lock()) {
        return false;
    }

    bool ok = false;
    if (g_ble_devices[device_index].in_use &&
        g_ble_devices[device_index].has_peer_addr) {
        *out = g_ble_devices[device_index].peer_addr;
        ok = true;
    }

    ble_state_unlock();
    return ok;
}

bool ble_runtime_set_peer_addr(int device_index, const ble_addr_t *addr)
{
    if (addr == NULL || device_index < 0 ||
        device_index >= DEVICE_STORE_MAX_DEVICES || !ble_state_lock()) {
        return false;
    }

    bool ok = false;
    if (g_ble_devices[device_index].in_use) {
        g_ble_devices[device_index].peer_addr = *addr;
        g_ble_devices[device_index].has_peer_addr = true;
        ok = true;
    }

    ble_state_unlock();
    return ok;
}

bool ble_runtime_get_device_id(int device_index, char *out, size_t cap)
{
    if (out == NULL || cap == 0 || device_index < 0 ||
        device_index >= DEVICE_STORE_MAX_DEVICES || !ble_state_lock()) {
        return false;
    }

    bool ok = g_ble_devices[device_index].in_use;
    if (ok) {
        strlcpy(out, g_ble_devices[device_index].device_id, cap);
    }

    ble_state_unlock();
    return ok;
}

void ble_runtime_finalize_remove(int device_index)
{
    if (device_index < 0 || device_index >= DEVICE_STORE_MAX_DEVICES ||
        !ble_state_lock()) {
        return;
    }

    ble_device_runtime_t *dev = &g_ble_devices[device_index];
    if (dev->in_use && dev->state == BLE_DEVICE_REMOVING) {
        memset(dev, 0, sizeof(*dev));
    }

    ble_state_unlock();
}

void ble_scheduler_note_success(int device_index)
{
    if (device_index < 0 || device_index >= DEVICE_STORE_MAX_DEVICES ||
        !ble_state_lock()) {
        return;
    }

    ble_device_runtime_t *dev = &g_ble_devices[device_index];
    if (dev->in_use && dev->state != BLE_DEVICE_REMOVING) {
        dev->retry_count = 0;
        dev->last_attempt_ms = ble_now_ms();
        dev->next_retry_ms = 0;
    }

    ble_state_unlock();
}

int ble_scheduler_next_device(int64_t now_ms)
{
    if (!ble_state_lock()) return BLE_CENTRAL_ERR_STATE;

    static int s_scheduler_cursor;

    int selected = BLE_CENTRAL_ERR_NOT_FOUND;
    for (int n = 0; n < DEVICE_STORE_MAX_DEVICES; n++) {
        int i = (s_scheduler_cursor + n) % DEVICE_STORE_MAX_DEVICES;
        const ble_device_runtime_t *dev = &g_ble_devices[i];
        if (!dev->in_use || !dev->reconnect_enabled) continue;
        if (dev->state != BLE_DEVICE_OFFLINE &&
            dev->state != BLE_DEVICE_BACKOFF) {
            continue;
        }
        if (dev->connection_slot != -1) continue;
        if (!dev->has_peer_addr) continue;
        if (now_ms < dev->next_retry_ms) continue;

        selected = i;
        s_scheduler_cursor = (i + 1) % DEVICE_STORE_MAX_DEVICES;
        break;
    }

    ble_state_unlock();
    return selected;
}

void ble_scheduler_note_failure(int device_index, int64_t now_ms)
{
    if (device_index < 0 || device_index >= DEVICE_STORE_MAX_DEVICES ||
        !ble_state_lock()) {
        return;
    }

    ble_device_runtime_t *dev = &g_ble_devices[device_index];
    if (dev->in_use && dev->state == BLE_DEVICE_CONNECTING) {
        int slot_index = dev->connection_slot;
        dev->connection_slot = -1;
        if (slot_index >= 0 && slot_index < BLE_CENTRAL_MAX_CONN &&
            g_ble_connections[slot_index].device_index == device_index) {
            ble_state_reset_slot_free_unlocked(slot_index);
        }
        dev->state = BLE_DEVICE_BACKOFF;
        int64_t delay = ble_backoff_delay_ms(dev->retry_count);
        dev->retry_count++;
        dev->last_attempt_ms = now_ms;
        dev->next_retry_ms = now_ms + delay;
    }

    ble_state_unlock();
}
