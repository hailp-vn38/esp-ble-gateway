#ifndef COMMAND_EXECUTOR_H
#define COMMAND_EXECUTOR_H

#include <stdint.h>

#include "esp_err.h"

#include "command_dispatcher.h"

// RAM-only runtime metrics (Plan v2 §18). Never persisted.
typedef struct {
    uint32_t submitted;
    uint32_t completed;
    uint32_t queue_full;
    uint32_t queue_timeout;
    uint32_t dispatch_timeout;
    uint32_t max_queue_depth;
    uint32_t active_workers;
} command_executor_stats_t;

// Result lifetime contract (Plan v2 §12): the result pointer is valid only
// while the callback runs. Do not store it, free it, or use it after the
// callback returns; copy if the data is needed longer.
typedef void (*command_completion_fn)(const dispatch_result_t *result,
                                      void *context);

// Single-shot, like command_dispatcher_init(). Creates the bounded queue and
// persistent workers (Plan v2 §11). Call after ble_central_init() and before
// web_server_start() (Plan v2 §70).
esp_err_t command_executor_init(void);

// Plan v2 §69 shutdown sequence: stop accepting jobs, signal workers with
// poison pills (queued jobs drain first), wait for workers to exit, then free
// resources. Safe to call when not initialized; leaks instead of faulting if a
// worker stays blocked past the wait budget.
void command_executor_deinit(void);

// Queue admission only: returns ESP_OK once queued, ESP_ERR_NO_MEM when the
// bounded queue is full (HTTP maps this to 503), ESP_ERR_INVALID_STATE after
// deinit, ESP_ERR_INVALID_ARG on bad arguments. Jobs that exceed
// CONFIG_CMD_EXEC_JOB_TIMEOUT_MS while waiting are completed with
// DISPATCH_STATUS_TIMEOUT without being dispatched (Plan v2 §16).
esp_err_t command_executor_submit(const gw_message_t *message,
                                  command_completion_fn completion,
                                  void *context);

void command_executor_get_stats(command_executor_stats_t *stats);

#endif // COMMAND_EXECUTOR_H
