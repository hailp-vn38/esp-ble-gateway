#include "command_executor.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define COMMAND_EXECUTOR_PRIORITY (tskIDLE_PRIORITY + 4)
#define DEINIT_WAIT_BUDGET_MS     10000

static const char *TAG = "command_executor";

// Queue item (Plan v2 §10): deliberately excludes dispatch_result_t so the
// queue stays small; workers own their result buffers instead.
typedef struct {
    gw_message_t message;
    command_completion_fn completion;
    void *context;
    int64_t submitted_at_us;
    int64_t deadline_us;
} command_job_t;

// Persistent per-worker result storage keeps memory deterministic and the
// worker stack small (Plan v2 §11).
typedef struct {
    TaskHandle_t task;
    dispatch_result_t result;
    uint32_t jobs_processed;
} command_worker_t;

static QueueHandle_t s_queue;
static command_worker_t s_workers[CONFIG_CMD_EXEC_WORKER_COUNT];
static volatile bool s_running;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static command_executor_stats_t s_stats;

static void stats_increment(uint32_t *field)
{
    taskENTER_CRITICAL(&s_stats_mux);
    (*field)++;
    taskEXIT_CRITICAL(&s_stats_mux);
}

static void stats_decrement(uint32_t *field)
{
    taskENTER_CRITICAL(&s_stats_mux);
    (*field)--;
    taskEXIT_CRITICAL(&s_stats_mux);
}

static void complete_job(command_worker_t *worker, const command_job_t *job)
{
    job->completion(&worker->result, job->context);
}

static void complete_expired_job(command_worker_t *worker,
                                 const command_job_t *job)
{
    dispatch_result_t *result = &worker->result;
    memset(result, 0, sizeof(*result));
    result->status = DISPATCH_STATUS_TIMEOUT;
    result->format = DISPATCH_RESULT_TEXT;
    strlcpy(result->payload, "Command expired in the executor queue",
            sizeof(result->payload));
    complete_job(worker, job);
}

static void worker_loop(void *arg)
{
    command_worker_t *worker = arg;
    command_job_t job;

    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        if (job.completion == NULL) break; // shutdown pill

        int64_t now_us = esp_timer_get_time();
        uint32_t wait_ms = (uint32_t)((now_us - job.submitted_at_us) / 1000);
        taskENTER_CRITICAL(&s_stats_mux);
        if (wait_ms > s_stats.max_queue_wait_ms) {
            s_stats.max_queue_wait_ms = wait_ms;
        }
        taskEXIT_CRITICAL(&s_stats_mux);

        if (now_us >= job.deadline_us) {
            stats_increment(&s_stats.queue_timeout);
            complete_expired_job(worker, &job);
            continue;
        }

        command_dispatcher_handle(&job.message, &worker->result);
        worker->jobs_processed++;
        stats_increment(&s_stats.completed);
        if (worker->result.status == DISPATCH_STATUS_TIMEOUT) {
            stats_increment(&s_stats.dispatch_timeout);
        }
        complete_job(worker, &job);
    }

    stats_decrement(&s_stats.active_workers);
    worker->task = NULL;
    vTaskDelete(NULL);
}

esp_err_t command_executor_init(void)
{
    if (s_queue != NULL) return ESP_ERR_INVALID_STATE;

    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_workers, 0, sizeof(s_workers));

    s_queue = xQueueCreate(CONFIG_CMD_EXEC_QUEUE_LEN, sizeof(command_job_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;

    s_running = true;
    for (size_t i = 0; i < CONFIG_CMD_EXEC_WORKER_COUNT; i++) {
        BaseType_t created =
            xTaskCreate(worker_loop, "cmd_exec", CONFIG_CMD_EXEC_WORKER_STACK,
                        &s_workers[i], COMMAND_EXECUTOR_PRIORITY,
                        &s_workers[i].task);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Could not create command worker %u", (unsigned)i);
            command_executor_deinit();
            return ESP_ERR_NO_MEM;
        }
        stats_increment(&s_stats.active_workers);
    }

    ESP_LOGI(TAG, "Command executor started: %u workers, queue %u",
             (unsigned)CONFIG_CMD_EXEC_WORKER_COUNT,
             (unsigned)CONFIG_CMD_EXEC_QUEUE_LEN);
    return ESP_OK;
}

void command_executor_deinit(void)
{
    if (s_queue == NULL) return;
    s_running = false;

    // Workers consume the backlog FIFO, then see their pills: queued jobs are
    // completed (dispatched or expired) before resources are freed.
    command_job_t pill = {.completion = NULL};
    size_t pills_needed = CONFIG_CMD_EXEC_WORKER_COUNT;
    for (int waited_ms = 0;
         (pills_needed > 0 || s_stats.active_workers > 0) &&
         waited_ms < DEINIT_WAIT_BUDGET_MS;
         waited_ms += 10) {
        while (pills_needed > 0 && xQueueSend(s_queue, &pill, 0) == pdTRUE) {
            pills_needed--;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (pills_needed > 0 || s_stats.active_workers > 0) {
        ESP_LOGE(TAG, "Executor workers did not stop; leaking resources");
        return;
    }

    vQueueDelete(s_queue);
    s_queue = NULL;
    ESP_LOGI(TAG, "Command executor stopped");
}

esp_err_t command_executor_submit(const gw_message_t *message,
                                  command_completion_fn completion,
                                  void *context)
{
    if (message == NULL || completion == NULL) return ESP_ERR_INVALID_ARG;
    if (s_queue == NULL || !s_running) return ESP_ERR_INVALID_STATE;

    command_job_t job = {
        .message = *message,
        .completion = completion,
        .context = context,
        .submitted_at_us = esp_timer_get_time(),
        .deadline_us =
            esp_timer_get_time() + CONFIG_CMD_EXEC_JOB_TIMEOUT_MS * 1000LL,
    };
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        stats_increment(&s_stats.queue_full);
        return ESP_ERR_NO_MEM;
    }

    stats_increment(&s_stats.submitted);
    UBaseType_t depth = uxQueueMessagesWaiting(s_queue);
    if ((uint32_t)depth > s_stats.max_queue_depth) {
        taskENTER_CRITICAL(&s_stats_mux);
        if ((uint32_t)depth > s_stats.max_queue_depth) {
            s_stats.max_queue_depth = depth;
        }
        taskEXIT_CRITICAL(&s_stats_mux);
    }
    return ESP_OK;
}

void command_executor_get_stats(command_executor_stats_t *stats,
                                uint32_t *worker_stack_min_bytes)
{
    if (worker_stack_min_bytes != NULL) *worker_stack_min_bytes = 0;
    if (stats == NULL) return;
    taskENTER_CRITICAL(&s_stats_mux);
    *stats = s_stats;
    taskEXIT_CRITICAL(&s_stats_mux);

    if (worker_stack_min_bytes != NULL) {
        uint32_t min_bytes = 0;
        for (size_t i = 0; i < CONFIG_CMD_EXEC_WORKER_COUNT; i++) {
            TaskHandle_t task = s_workers[i].task;
            if (task == NULL) continue;
            UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(task);
            uint32_t bytes = (uint32_t)free_bytes;
            if (min_bytes == 0 || bytes < min_bytes) min_bytes = bytes;
        }
        *worker_stack_min_bytes = min_bytes;
    }
}
