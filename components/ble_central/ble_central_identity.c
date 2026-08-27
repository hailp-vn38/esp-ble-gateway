#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_central_internal.h"

// Deferred BLE identity persistence (refactor plan §6).
//
// NimBLE host callbacks must never block on NVS. The GAP callback only
// submits the canonical peer identity here (bounded, non-blocking copy);
// a dedicated worker owns the actual device_store_set_ble_identity() call,
// with dirty-state retry so a full/failed moment can never permanently
// lose an identity update.

#define IDENTITY_TASK_STACK       3072
#define IDENTITY_TASK_PRIORITY       3
#define IDENTITY_RETRY_DELAY_MS   1000

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    uint8_t addr[6];
    uint8_t addr_type;
    uint32_t generation;
    bool dirty;
} identity_pending_t;

static const char *TAG = "ble_central_ident";

static identity_pending_t s_pending[DEVICE_STORE_MAX_DEVICES];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;

// Called from the NimBLE host task: bounded work only, no blocking, no NVS.
void ble_central_identity_submit(const char *device_id, const ble_addr_t *addr)
{
    if (device_id == NULL || addr == NULL || s_task == NULL) return;

    bool accepted = false;
    uint32_t generation = 0;

    portENTER_CRITICAL(&s_lock);
    identity_pending_t *slot = NULL;
    for (size_t i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if ((s_pending[i].dirty || s_pending[i].generation > 0) &&
            strcmp(s_pending[i].device_id, device_id) == 0) {
            slot = &s_pending[i]; // Coalesce: latest value wins.
            break;
        }
        if (slot == NULL && s_pending[i].generation == 0) {
            slot = &s_pending[i]; // Never-used slot: claim it.
        }
    }
    if (slot != NULL) {
        if (slot->generation == 0) {
            strlcpy(slot->device_id, device_id, sizeof(slot->device_id));
        }
        memcpy(slot->addr, addr->val, sizeof(slot->addr));
        slot->addr_type = addr->type;
        slot->generation++;
        slot->dirty = true;
        generation = slot->generation;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!accepted) {
        ble_central_metrics_identity_persist_failure();
        ESP_LOGW(TAG, "[%s] No pending slot for identity update", device_id);
        return;
    }

    ESP_LOGD(TAG, "[%s] Identity update queued (gen=%u)", device_id,
             (unsigned)generation);
    xTaskNotifyGive(s_task);
}

static bool identity_take_next(identity_pending_t *out)
{
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        if (!s_pending[i].dirty) continue;
        *out = s_pending[i];
        found = true;
        break;
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}

static void identity_complete(const identity_pending_t *snapshot, bool success)
{
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < DEVICE_STORE_MAX_DEVICES; i++) {
        identity_pending_t *slot = &s_pending[i];
        // Only retire this exact update; newer submissions stay dirty.
        if (slot->dirty && slot->generation == snapshot->generation &&
            strcmp(slot->device_id, snapshot->device_id) == 0) {
            if (success) slot->dirty = false;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

static void identity_worker(void *arg)
{
    (void)arg;
    TickType_t idle_timeout = portMAX_DELAY;
    identity_pending_t snapshot;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, idle_timeout);

        bool failed = false;
        while (identity_take_next(&snapshot)) {
            device_store_result_t result = device_store_set_ble_identity(
                snapshot.device_id, snapshot.addr, snapshot.addr_type);
            if (result == DEVICE_STORE_OK ||
                result == DEVICE_STORE_ERR_DUPLICATE_BLE_IDENTITY ||
                result == DEVICE_STORE_ERR_NOT_FOUND) {
                // Terminal outcomes: retrying cannot change the answer.
                if (result != DEVICE_STORE_OK) {
                    ESP_LOGW(TAG, "[%s] Identity persist dropped: %d",
                             snapshot.device_id, result);
                }
                identity_complete(&snapshot, true);
            } else {
                ESP_LOGE(TAG, "[%s] Identity persist failed (%d), will retry",
                         snapshot.device_id, result);
                ble_central_metrics_identity_persist_failure();
                failed = true;
                // Keep dirty; leave the loop so the backoff below applies
                // before hammering NVS again.
                break;
            }
        }

        idle_timeout =
            failed ? pdMS_TO_TICKS(IDENTITY_RETRY_DELAY_MS) : portMAX_DELAY;
    }
}

int ble_central_identity_init(void)
{
    if (s_task != NULL) return 0;
    if (xTaskCreate(identity_worker, "ble_ident", IDENTITY_TASK_STACK, NULL,
                    IDENTITY_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return -1;
    }
    return 0;
}
