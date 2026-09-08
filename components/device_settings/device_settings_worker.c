/**
 * Settings Worker and Coordinator (G3)
 *
 * Drives queued DESCRIBE/GET settings operations into real BLE command
 * submissions via the device command service.  Enforces global
 * serialization (one active settings stream), bounded BUSY retry,
 * disconnect cleanup, and duplicate coalescing.
 */

#include <string.h>

#include "device_command_service.h"
#include "device_settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "gw_settings_view.h"

static const char *TAG = "ds_worker";

/* ── Event bits ──────────────────────────────────────────────────────── */

#define DS_WORK_BIT_WORK_QUEUED  BIT0
#define DS_WORK_BIT_CMD_COMPLETE BIT1
#define DS_WORK_BIT_STOP         BIT2

/* ── Retry backoff (ms) ──────────────────────────────────────────────── */

static const uint32_t s_retry_backoff_ms[] = {100, 250, 500};
#define DS_WORK_RETRY_MAX \
    (sizeof(s_retry_backoff_ms) / sizeof(s_retry_backoff_ms[0]))

/* ── Active operation (global, one at a time) ────────────────────────── */

typedef struct {
    bool       active;
    bool       advance_to_read;
    int        slot;     /* index into s_ops[] */
    uint32_t   generation;
    uint8_t    retry_count;
} ds_active_op_t;

static ds_active_op_t s_active_op;

/* ── Worker state ────────────────────────────────────────────────────── */

typedef struct {
    TaskHandle_t      task;
    EventGroupHandle_t events;
    volatile bool     running;
    TimerHandle_t     retry_timer;
} ds_worker_t;

static ds_worker_t s_worker;

/* ── Forward declarations ────────────────────────────────────────────── */

static void worker_task(void *arg);
static void on_cmd_complete(const device_command_result_t *result,
                            void *context);

/* ── Find first queued operation (external s_ops[] from operation.c) ─── */

extern bool      s_ops_active[];
extern uint32_t  s_ops_generation[];
extern void      s_ops_invoke_completion(int slot,
                                         ds_op_result_t result);
extern const char *s_ops_get_device_id(int slot);
extern ds_op_kind_t s_ops_get_kind(int slot);
extern void      s_ops_set_state(int slot, ds_op_state_t state);

static int find_first_queued(uint32_t min_generation)
{
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_ops_active[i] &&
            s_ops_generation[i] >= min_generation) {
            return i;
        }
    }
    return -1;
}

static bool __attribute__((unused))
is_duplicate_queued(const char *device_id,
                    ds_op_kind_t kind,
                    uint32_t generation)
{
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_ops_active[i] &&
            s_ops_generation[i] == generation &&
            strncmp(s_ops_get_device_id(i), device_id,
                    GW_MSG_DEVICE_ID_LEN) == 0 &&
            s_ops_get_kind(i) == kind) {
            return true;
        }
    }
    return false;
}

/* ── Command submission ──────────────────────────────────────────────── */

static esp_err_t submit_command(int slot)
{
    const char *device_id = s_ops_get_device_id(slot);
    ds_op_kind_t kind = s_ops_get_kind(slot);

    device_command_request_t req = {0};
    req.origin = DEVICE_CMD_ORIGIN_SETTINGS;
    strlcpy(req.device_id, device_id, sizeof(req.device_id));

    switch (kind) {
    case DS_OP_DESCRIBE:
        strlcpy(req.command, GW_SETTINGS_CMD_DESCRIBE_SETTINGS,
                sizeof(req.command));
        break;
    case DS_OP_GET:
        strlcpy(req.command, GW_SETTINGS_CMD_READ_SETTINGS,
                sizeof(req.command));
        break;
    default:
        ESP_LOGW(TAG, "[%s] unknown op kind %d", device_id, (int)kind);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = device_command_service_submit(
        &req, on_cmd_complete, (void *)(intptr_t)slot);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] submit %s failed: %s",
                 device_id, req.command, esp_err_to_name(err));
    }
    return err;
}

/* ── Worker-side completion ────────────────────────────────────────────
 * Completes the operation without touching op_state (avoids racing with
 * on_cmd_complete which checks s_active_op.active). */

static void worker_complete_op(int slot, ds_op_result_t result)
{
    const char *device_id = s_ops_get_device_id(slot);
    s_ops_invoke_completion(slot, result);
    ESP_LOGI(TAG, "[%s] [COMPLETE] op=%d result=%d",
             device_id, slot, (int)result);
}

/* ── Retry timer callback (runs from timer daemon) ───────────────────── */

static void retry_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (s_worker.events != NULL) {
        xEventGroupSetBits(s_worker.events, DS_WORK_BIT_CMD_COMPLETE);
    }
}

/* ── Command completion callback (runs from command service task) ─────── */

static void on_cmd_complete(const device_command_result_t *result,
                            void *context)
{
    int slot = (int)(intptr_t)context;

    if (!s_worker.running) return;
    if (!s_active_op.active || s_active_op.slot != slot) return;

    if (result == NULL) {
        ESP_LOGE(TAG, "cmd callback NULL result slot=%d", slot);
        s_active_op.active = false;
        worker_complete_op(slot, DS_OP_RESULT_INTERNAL);
        if (s_worker.events != NULL) {
            xEventGroupSetBits(s_worker.events,
                               DS_WORK_BIT_CMD_COMPLETE);
        }
        return;
    }

    ds_op_kind_t kind = s_ops_get_kind(slot);
    const char *device_id = s_ops_get_device_id(slot);

    switch (result->status) {
    case DEVICE_CMD_STATUS_OK:
        if (kind == DS_OP_DESCRIBE) {
            /* DESCRIBE ACK received — mark QUEUED and flag that the worker
             * task should advance this op to READ once re-awakened.
             * (Doing it here in the callback would race with the task). */
            ESP_LOGI(TAG, "[%s] [DESCRIBE_ACK] slot=%d",
                     device_id, slot);
            s_active_op.advance_to_read = true;
            s_ops_set_state(slot, DS_OP_QUEUED);
        } else {
            /* READ ACK — operation complete. */
            ESP_LOGI(TAG, "[%s] [READ_ACK] slot=%d",
                     device_id, slot);
            s_active_op.active = false;
            worker_complete_op(slot, DS_OP_RESULT_OK);
            DS_DIAG_INC(discovery_values_success);
        }
        break;

    case DEVICE_CMD_STATUS_BUSY:
        ESP_LOGW(TAG, "[%s] [BUSY] slot=%d retry=%d/%d",
                 device_id, slot,
                 (int)s_active_op.retry_count,
                 (int)DS_WORK_RETRY_MAX);
        s_ops_set_state(slot, DS_OP_QUEUED);
        if (s_active_op.retry_count < DS_WORK_RETRY_MAX) {
            uint32_t delay = s_retry_backoff_ms[s_active_op.retry_count];
            s_active_op.retry_count++;
            if (s_worker.retry_timer != NULL) {
                xTimerChangePeriod(s_worker.retry_timer,
                                   pdMS_TO_TICKS(delay), 0);
                xTimerStart(s_worker.retry_timer, 0);
            }
            DS_DIAG_INC(tx_fail);
        } else {
            ESP_LOGE(TAG, "[%s] retry exhausted slot=%d",
                     device_id, slot);
            s_active_op.active = false;
            worker_complete_op(slot, DS_OP_RESULT_FAILED);
            DS_DIAG_INC(tx_fail);
        }
        break;

    case DEVICE_CMD_STATUS_TIMEOUT:
        ESP_LOGW(TAG, "[%s] [TIMEOUT] slot=%d", device_id, slot);
        s_active_op.active = false;
        worker_complete_op(slot, DS_OP_RESULT_TIMEOUT);
        DS_DIAG_INC(tx_fail);
        break;

    case DEVICE_CMD_STATUS_NOT_CONNECTED:
        ESP_LOGW(TAG, "[%s] [NOT_CONNECTED] slot=%d", device_id, slot);
        s_active_op.active = false;
        worker_complete_op(slot, DS_OP_RESULT_TIMEOUT);
        DS_DIAG_INC(tx_fail);
        break;

    case DEVICE_CMD_STATUS_REJECTED:
        ESP_LOGW(TAG, "[%s] [REJECTED] slot=%d", device_id, slot);
        s_active_op.active = false;
        worker_complete_op(slot, DS_OP_RESULT_REJECTED);
        DS_DIAG_INC(tx_fail);
        break;

    default:
        ESP_LOGW(TAG, "[%s] [ERROR status=%d] slot=%d",
                 device_id, (int)result->status, slot);
        s_active_op.active = false;
        worker_complete_op(slot, DS_OP_RESULT_INTERNAL);
        DS_DIAG_INC(tx_fail);
        break;
    }

    /* Signal worker to process next queued op. */
    if (s_worker.events != NULL) {
        xEventGroupSetBits(s_worker.events, DS_WORK_BIT_CMD_COMPLETE);
    }
}

/* ── Worker task ─────────────────────────────────────────────────────── */

static void worker_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[START] worker task started");

    while (s_worker.running) {
        EventBits_t bits = xEventGroupWaitBits(
            s_worker.events,
            DS_WORK_BIT_WORK_QUEUED | DS_WORK_BIT_CMD_COMPLETE |
                DS_WORK_BIT_STOP,
            pdTRUE,   /* clear on exit */
            pdFALSE,  /* wait for any bit */
            portMAX_DELAY);

        if ((bits & DS_WORK_BIT_STOP) != 0) break;

        /* Process active operation completion. */
        if ((bits & DS_WORK_BIT_CMD_COMPLETE) != 0 &&
            s_active_op.active && s_active_op.advance_to_read) {
            s_active_op.advance_to_read = false;
            int slot = s_active_op.slot;
            if (slot >= 0 && s_ops_active[slot] &&
                s_ops_get_kind(slot) == DS_OP_DESCRIBE &&
                s_ops_get_state(slot) == DS_OP_QUEUED) {
                s_active_op.generation = s_ops_generation[slot];
                s_active_op.retry_count = 0;
                /* Advance from DESCRIBE phase to GET/READ phase. */
                s_ops_set_kind(slot, DS_OP_GET);
                esp_err_t err = submit_command(slot);
                if (err == ESP_OK) {
                    s_ops_set_state(slot, DS_OP_RUNNING);
                    ESP_LOGI(TAG, "[%s] [START] READ after DESCRIBE_ACK",
                             s_ops_get_device_id(slot));
                } else {
                    s_active_op.active = false;
                    worker_complete_op(slot, DS_OP_RESULT_INTERNAL);
                }
            }
        }

        /* Find and start next queued operation (if none active). */
        if (!s_active_op.active) {
            int slot = find_first_queued(0);
            if (slot >= 0) {
                s_active_op.active = true;
                s_active_op.slot = slot;
                s_active_op.generation = s_ops_generation[slot];
                s_active_op.retry_count = 0;

                s_ops_set_state(slot, DS_OP_RUNNING);
                esp_err_t err = submit_command(slot);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "[%s] [START] %s op=%d",
                             s_ops_get_device_id(slot),
                             s_ops_get_kind(slot) == DS_OP_DESCRIBE
                                 ? "DESCRIBE" : "READ",
                             slot);
                } else {
                    s_active_op.active = false;
                    worker_complete_op(slot, DS_OP_RESULT_INTERNAL);
                }
            }
        }
    }

    ESP_LOGI(TAG, "[STOP] worker task exiting");
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t device_settings_worker_init(void)
{
    if (s_worker.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_active_op, 0, sizeof(s_active_op));

    s_worker.events = xEventGroupCreate();
    if (s_worker.events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_worker.retry_timer = xTimerCreate(
        "ds_wrk_retry",
        pdMS_TO_TICKS(100),
        pdFALSE,   /* one-shot */
        NULL,
        retry_timer_cb);
    if (s_worker.retry_timer == NULL) {
        vEventGroupDelete(s_worker.events);
        s_worker.events = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_worker.running = true;

    BaseType_t created = xTaskCreate(
        worker_task, "ds_worker", 4096, NULL, 4, &s_worker.task);
    if (created != pdPASS) {
        s_worker.running = false;
        xTimerDelete(s_worker.retry_timer, 0);
        s_worker.retry_timer = NULL;
        vEventGroupDelete(s_worker.events);
        s_worker.events = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[INIT] worker initialized");
    return ESP_OK;
}

void device_settings_worker_deinit(void)
{
    if (s_worker.task == NULL) return;

    s_worker.running = false;

    if (s_worker.retry_timer != NULL) {
        xTimerStop(s_worker.retry_timer, 0);
        xTimerDelete(s_worker.retry_timer, 0);
        s_worker.retry_timer = NULL;
    }

    if (s_worker.events != NULL) {
        xEventGroupSetBits(s_worker.events, DS_WORK_BIT_STOP);
    }

    /* Wait for task to self-delete (bounded). */
    for (int i = 0; i < 50 && s_worker.task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_worker.events != NULL) {
        vEventGroupDelete(s_worker.events);
        s_worker.events = NULL;
    }

    memset(&s_active_op, 0, sizeof(s_active_op));
    s_worker.task = NULL;
    ESP_LOGI(TAG, "[DEINIT] worker deinitialized");
}

void device_settings_worker_submit(void)
{
    if (s_worker.events == NULL || !s_worker.running) return;
    xEventGroupSetBits(s_worker.events, DS_WORK_BIT_WORK_QUEUED);
}

void device_settings_worker_on_disconnect(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return;

    /* Cancel active command if it belongs to this device. */
    if (s_active_op.active && s_active_op.slot >= 0) {
        const char *active_id =
            s_ops_get_device_id(s_active_op.slot);
        if (active_id != NULL &&
            strncmp(active_id, device_id,
                    GW_MSG_DEVICE_ID_LEN) == 0) {
            ESP_LOGI(TAG, "[%s] [DISCONNECT] cancelling active cmd",
                     device_id);
            device_command_service_cancel_device(device_id);
            s_active_op.active = false;
        }
    }

    /* Increment generation to invalidate pending operations. */
    for (int i = 0; i < DEVICE_SETTINGS_MAX_DEVICES; i++) {
        if (s_ops_active[i] &&
            strncmp(s_ops_get_device_id(i), device_id,
                    GW_MSG_DEVICE_ID_LEN) == 0) {
            s_ops_generation[i]++;
            ESP_LOGD(TAG, "[%s] [DISCONNECT] incremented gen slot=%d",
                     device_id, i);
        }
    }

    /* Signal worker to process remaining queued ops. */
    if (s_worker.events != NULL) {
        xEventGroupSetBits(s_worker.events, DS_WORK_BIT_WORK_QUEUED);
    }
}

/* ── Test helpers ────────────────────────────────────────────────────── */

void device_settings_worker_reset_for_test(void)
{
    if (s_worker.retry_timer != NULL) {
        xTimerStop(s_worker.retry_timer, 0);
    }
    memset(&s_active_op, 0, sizeof(s_active_op));
    if (s_worker.events != NULL) {
        xEventGroupClearBits(
            s_worker.events,
            DS_WORK_BIT_WORK_QUEUED | DS_WORK_BIT_CMD_COMPLETE);
    }
}

bool device_settings_worker_is_idle_for_test(void)
{
    return !s_active_op.active;
}

uint32_t device_settings_worker_get_generation_for_test(void)
{
    return s_active_op.generation;
}
