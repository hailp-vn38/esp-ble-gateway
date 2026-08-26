#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"

#include "ble_central_internal.h"

static const char *TAG = "ble_central_supervisor";

typedef enum {
    BLE_SUPERVISOR_STOPPED = 0,
    BLE_SUPERVISOR_RUNNING,
    BLE_SUPERVISOR_STOPPING,
} ble_supervisor_state_t;

static ble_supervisor_state_t s_supervisor_state;
static TaskHandle_t s_supervisor_task;

static bool supervisor_state_is(ble_supervisor_state_t state)
{
    bool match = false;
    if (ble_state_lock()) {
        match = s_supervisor_state == state;
        ble_state_unlock();
    }
    return match;
}

static void terminate_timed_out_connections(int64_t now_ms)
{
    ble_timeout_entry_t entries[BLE_CENTRAL_MAX_CONN];
    size_t count =
        ble_state_collect_timeouts(now_ms, entries, BLE_CENTRAL_MAX_CONN);

    for (size_t i = 0; i < count; i++) {
        ESP_LOGE(TAG, "[%s] %s timeout",
                 entries[i].device_id,
                 entries[i].is_discovery ? "GATT discovery" : "Security");
        if (entries[i].is_discovery) {
            ble_central_metrics_discovery_failure();
        } else {
            ble_central_metrics_security_failure();
        }
        ble_gap_terminate(entries[i].conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void reconnect_supervisor_task(void *arg)
{
    (void)arg;
    while (supervisor_state_is(BLE_SUPERVISOR_RUNNING)) {
        int64_t now_ms = ble_now_ms();

        terminate_timed_out_connections(now_ms);

        if (!ble_host_is_ready() || ble_gap_disc_active()) {
            goto sleep;
        }

        int device_index = ble_scheduler_next_device(now_ms);
        if (device_index >= 0) {
            int rc = ble_connection_start(device_index);
            if (rc != BLE_CENTRAL_OK &&
                rc != BLE_CENTRAL_ERR_CONNECT_IN_PROGRESS &&
                rc != BLE_CENTRAL_ERR_NO_SLOT &&
                rc != BLE_CENTRAL_ERR_STACK) {
                ble_scheduler_note_failure(device_index, now_ms);
            }
        }

sleep:
        vTaskDelay(pdMS_TO_TICKS(BLE_SUPERVISOR_TICK_MS));
    }

    if (ble_state_lock()) {
        s_supervisor_state = BLE_SUPERVISOR_STOPPED;
        s_supervisor_task = NULL;
        ble_state_unlock();
    }
    ESP_LOGI(TAG, "Reconnect supervisor stopped");
    vTaskDelete(NULL);
}

int ble_central_start_reconnect_supervisor(void)
{
    if (!ble_state_lock()) return -1;
    if (s_supervisor_state != BLE_SUPERVISOR_STOPPED) {
        bool running = s_supervisor_state == BLE_SUPERVISOR_RUNNING;
        ble_state_unlock();
        return running ? 0 : -1;
    }
    s_supervisor_state = BLE_SUPERVISOR_RUNNING;
    ble_state_unlock();

    BaseType_t result = xTaskCreate(reconnect_supervisor_task, "ble_reconnect",
                                    4096, NULL, 4, &s_supervisor_task);
    if (result != pdPASS) {
        if (ble_state_lock()) {
            s_supervisor_state = BLE_SUPERVISOR_STOPPED;
            s_supervisor_task = NULL;
            ble_state_unlock();
        }
        return -1;
    }

    ESP_LOGI(TAG, "Reconnect supervisor started");
    return 0;
}

void ble_central_stop_reconnect_supervisor(void)
{
    if (!ble_state_lock()) return;
    if (s_supervisor_state == BLE_SUPERVISOR_RUNNING) {
        s_supervisor_state = BLE_SUPERVISOR_STOPPING;
    }
    ble_state_unlock();
}
