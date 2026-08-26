#include "board_status_sync.h"

#include <stdbool.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_io.h"
#include "wifi_prov.h"

static const char *TAG = "board_status_sync";

#define SYNC_INTERVAL_MS 300U
#define SYNC_TASK_STACK_SIZE 3072U
#define SYNC_TASK_PRIORITY 2U

static board_status_t resolve_status(wifi_prov_state_t wifi_state)
{
    switch (wifi_state) {
    case WIFI_PROV_STATE_BOOT_CONNECTING:
    case WIFI_PROV_STATE_RECONNECTING:
        return BOARD_STATUS_WIFI_CONNECTING;
    case WIFI_PROV_STATE_PROVISIONING:
    case WIFI_PROV_STATE_TESTING:
    case WIFI_PROV_STATE_RESTART_PENDING:
        return BOARD_STATUS_PROVISIONING;
    case WIFI_PROV_STATE_CONNECTED:
        return BOARD_STATUS_READY;
    case WIFI_PROV_STATE_FAILED:
        return BOARD_STATUS_ERROR;
    case WIFI_PROV_STATE_UNINITIALIZED:
    default:
        return BOARD_STATUS_BOOTING;
    }
}

static void sync_task(void *arg)
{
    (void)arg;
    board_status_t last_pushed = BOARD_STATUS_COUNT;

    for (;;) {
        board_status_t resolved = resolve_status(wifi_prov_get_state());
        if (resolved != last_pushed) {
            if (board_io_set_status(resolved) == ESP_OK) {
                ESP_LOGD(TAG, "Board status -> %d", (int)resolved);
                last_pushed = resolved;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SYNC_INTERVAL_MS));
    }
}

esp_err_t board_status_sync_start(void)
{
    if (xTaskCreate(sync_task, "board_st_sync", SYNC_TASK_STACK_SIZE,
                    NULL, SYNC_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
