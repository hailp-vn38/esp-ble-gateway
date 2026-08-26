#include <stdbool.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "sdkconfig.h"

#include "command_dispatcher.h"
#include "mcp_endpoint_internal.h"

// Single-worker async path for device_command: the BLE ACK wait (up to 2s)
// moves off the shared HTTPD task. Jobs are fully validated and normalized
// before submission, so the worker only dispatches, formats and answers.
//
// Socket budget: LWIP allows ~10 sockets; the HTTP server holds one listener
// plus one per connection. Queue length 2 + in-flight 1 means /mcp can pin at
// most 3 sockets, leaving headroom for the Web UI.

#define MCP_ASYNC_WORKERS 1
#define MCP_ASYNC_QUEUE_LEN 2

typedef struct {
    httpd_req_t *req;
    cJSON *id;
    gw_message_t msg;
    bool notification;
    mcp_request_meta_t meta;
} mcp_async_job_t;

static const char *TAG = "mcp_async";

static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static volatile bool s_running;

static void finish_job(httpd_req_t *req)
{
    if (mcp_transport_get()->async_complete(req) != ESP_OK) {
        ESP_LOGD(TAG, "async_complete failed (client gone?)");
    }
}

static void answer_job(mcp_async_job_t *job)
{
    if (job->notification) {
        cJSON_Delete(job->id);
        job->id = NULL;
        return;
    }
    mcp_rpc_error_t rpc_error = {0};
    cJSON *result = mcp_tools_execute(&job->msg, &job->meta, &rpc_error);
    if (result == NULL) {
        mcp_rpc_send_error_ex(job->req, rpc_error.code, rpc_error.message,
                              job->id, &job->meta, NULL, false);
    } else {
        mcp_rpc_send_result_ex(job->req, result, job->id, &job->meta);
    }
}

static void worker_loop(void *arg)
{
    (void)arg;
    mcp_async_job_t job;
    while (s_running) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        if (job.req == NULL) break; // poison pill on shutdown

        answer_job(&job);
        finish_job(job.req);
        cJSON_Delete(job.id);

        UBaseType_t high_water = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGD(TAG, "device_command done; stack high water %u bytes",
                 (unsigned)(high_water * sizeof(StackType_t)));
    }
    s_worker = NULL;
    vTaskDelete(NULL);
}

esp_err_t mcp_async_submit(httpd_req_t *req, cJSON *id, const gw_message_t *msg,
                           const mcp_request_meta_t *meta, bool notification)
{
    if (s_queue == NULL || !s_running) {
        // Caller keeps ownership of id on failure.
        return ESP_ERR_INVALID_STATE;
    }
    mcp_async_job_t job = {
        .req = req,
        .id = id,
        .msg = *msg,
        .notification = notification,
        .meta = *meta,
    };
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        // Caller keeps ownership of id on failure and answers synchronously.
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int mcp_async_init(void)
{
    if (s_queue != NULL && s_running) {
        mcp_async_deinit();
    } else if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }

    s_queue = xQueueCreate(MCP_ASYNC_QUEUE_LEN, sizeof(mcp_async_job_t));
    if (s_queue == NULL) return -1;

    s_running = true;
    BaseType_t created = xTaskCreate(worker_loop, "mcp_async",
                                     CONFIG_MCP_ASYNC_STACK / sizeof(StackType_t),
                                     NULL, tskIDLE_PRIORITY + 3, &s_worker);
    if (created != pdPASS) {
        s_running = false;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return -1;
    }
    return 0;
}

TaskHandle_t mcp_async_worker(void)
{
    return s_worker;
}

void mcp_async_deinit(void)
{
    if (s_queue == NULL) return;
    s_running = false;
    // Drain any queued jobs so a poison pill reaches the worker even when
    // callers abandoned requests during a failed registration.
    mcp_async_job_t job;
    while (uxQueueMessagesWaiting(s_queue) > 0) {
        if (xQueueReceive(s_queue, &job, 0) == pdTRUE && job.req != NULL) {
            cJSON_Delete(job.id);
            finish_job(job.req);
        }
    }
    mcp_async_job_t pill = {0};
    xQueueSend(s_queue, &pill, 0);
    // A resumed worker may still owe up to two 2s ACK waits before it sees
    // the pill; give it that budget before giving up.
    for (int i = 0; i < 700 && s_worker != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_worker != NULL) {
        // Worker still inside a dispatch; deleting the queue under it would
        // fault. Leak the queue instead — deinit only runs on a failed
        // registration where boot aborts anyway.
        ESP_LOGE(TAG, "async worker did not stop; leaking queue");
        return;
    }
    vQueueDelete(s_queue);
    s_queue = NULL;
}
