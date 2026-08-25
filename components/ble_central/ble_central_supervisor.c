#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"

#include "ble_central_internal.h"
#include "device_store.h"

static const char *TAG = "ble_central_supervisor";
static TaskHandle_t s_reconnect_task;
static volatile bool s_reconnect_running;

static bool snapshot_slot_state(const char *device_id,
                                ble_conn_slot_state_t *state,
                                int64_t *last_attempt_ms)
{
    bool found = false;
    if (ble_central_lock_connections()) {
        ble_conn_slot_t *slot = ble_central_find_slot_unlocked(device_id);
        if (slot != NULL) {
            *state = slot->state;
            *last_attempt_ms = slot->last_attempt_ms;
            found = true;
        }
        ble_central_unlock_connections();
    }
    return found;
}

static void terminate_timed_out_discoveries(int64_t now_ms)
{
    uint16_t timed_out_handles[BLE_CENTRAL_MAX_CONN];
    int timed_out_count = 0;
    if (!ble_central_lock_connections()) return;

    for (int i = 0; i < BLE_CENTRAL_MAX_CONN; i++) {
        if ((g_ble_connections[i].state == BLE_CONN_SLOT_SECURING ||
             g_ble_connections[i].state == BLE_CONN_SLOT_DISCOVERING) &&
            now_ms - g_ble_connections[i].discovery_started_ms >=
                BLE_DISCOVERY_TIMEOUT_MS) {
            timed_out_handles[timed_out_count++] =
                g_ble_connections[i].conn_handle;
            ESP_LOGE(TAG, "[%s] GATT discovery timed out",
                     g_ble_connections[i].device_id);
        }
    }
    ble_central_unlock_connections();

    for (int i = 0; i < timed_out_count; i++) {
        ble_gap_terminate(timed_out_handles[i], BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void reconnect_supervisor_task(void *arg)
{
    while (s_reconnect_running) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        terminate_timed_out_discoveries(now_ms);

        if (g_ble_host_synced && !ble_gap_disc_active()) {
            device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
            int count = device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES);
            for (int i = 0; i < count; i++) {
                if (devices[i].connected || !devices[i].has_ble_addr) continue;

                ble_conn_slot_state_t state = BLE_CONN_SLOT_IDLE;
                int64_t last_attempt_ms = 0;
                bool has_slot = snapshot_slot_state(
                    devices[i].device_id, &state, &last_attempt_ms);
                if (has_slot && state != BLE_CONN_SLOT_IDLE) continue;
                if (has_slot &&
                    now_ms - last_attempt_ms < BLE_RECONNECT_INTERVAL_MS) {
                    continue;
                }

                ble_central_connect(devices[i].device_id, devices[i].ble_addr,
                                    devices[i].ble_addr_type);
                /* NimBLE controllers commonly serialize connection procedures. */
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BLE_SUPERVISOR_TICK_MS));
    }

    s_reconnect_task = NULL;
    vTaskDelete(NULL);
}

int ble_central_start_reconnect_supervisor(void)
{
    if (s_reconnect_running) return 0;
    s_reconnect_running = true;
    BaseType_t result = xTaskCreate(reconnect_supervisor_task, "ble_reconnect",
                                    4096, NULL, 4, &s_reconnect_task);
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
