#include "device_schema_control_adapter.h"

#include <string.h>

#include "device_control_scheduler.h"
#include "device_schema.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define SCHEMA_CTRL_QUEUE_WAIT_MS 5000

/* Schema discovery is globally serialized by device_schema.  Keep the bridge
 * static as well: an accepted request has exactly one live context and no
 * heap allocation can outlive a scheduler cancellation. */
typedef struct {
    device_schema_submit_done_fn done;
    void *context;
    bool active;
} schema_ctrl_bridge_t;

static schema_ctrl_bridge_t s_bridge;
static portMUX_TYPE s_bridge_mux = portMUX_INITIALIZER_UNLOCKED;

static device_schema_submit_result_t map_command_result(
    const device_command_result_t *result)
{
    if (result == NULL) return DEVICE_SCHEMA_SUBMIT_INTERNAL_ERROR;
    switch (result->status) {
    case DEVICE_CMD_STATUS_OK:
        return DEVICE_SCHEMA_SUBMIT_OK;
    case DEVICE_CMD_STATUS_DEVICE_REJECTED:
    case DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND:
        return DEVICE_SCHEMA_SUBMIT_REJECTED;
    case DEVICE_CMD_STATUS_BUSY:
    case DEVICE_CMD_STATUS_QUEUE_FULL:
        return DEVICE_SCHEMA_SUBMIT_BUSY;
    case DEVICE_CMD_STATUS_TIMEOUT:
        return DEVICE_SCHEMA_SUBMIT_TIMEOUT;
    case DEVICE_CMD_STATUS_NOT_CONNECTED:
        return DEVICE_SCHEMA_SUBMIT_NOT_CONNECTED;
    case DEVICE_CMD_STATUS_TRANSPORT_ERROR:
        return DEVICE_SCHEMA_SUBMIT_TRANSPORT_ERROR;
    default:
        return DEVICE_SCHEMA_SUBMIT_INTERNAL_ERROR;
    }
}

static void schema_scheduler_done(const device_control_result_t *result,
                                  void *context)
{
    (void)context;
    device_schema_submit_done_fn done = NULL;
    void *done_context = NULL;
    device_schema_submit_result_t outcome = DEVICE_SCHEMA_SUBMIT_INTERNAL_ERROR;

    if (result != NULL) {
        if (result->terminal == DEVICE_CTRL_TERMINAL_DEADLINE_EXCEEDED) {
            outcome = DEVICE_SCHEMA_SUBMIT_TIMEOUT;
        } else if (result->terminal == DEVICE_CTRL_TERMINAL_CANCELLED ||
                   result->terminal == DEVICE_CTRL_TERMINAL_SUPERSEDED) {
            outcome = DEVICE_SCHEMA_SUBMIT_BUSY;
        } else {
            outcome = map_command_result(&result->command_result);
        }
    }

    taskENTER_CRITICAL(&s_bridge_mux);
    done = s_bridge.done;
    done_context = s_bridge.context;
    memset(&s_bridge, 0, sizeof(s_bridge));
    taskEXIT_CRITICAL(&s_bridge_mux);

    /* Schema posts its event from here; never hold the bridge lock over it. */
    if (done != NULL) done(outcome, done_context);
}

static esp_err_t schema_scheduler_submit(const gw_message_t *message,
                                         device_schema_submit_done_fn done,
                                         void *context)
{
    if (message == NULL || done == NULL || !message->has_device_id ||
        message->device_id[0] == '\0' || message->command[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_bridge_mux);
    if (s_bridge.active) {
        taskEXIT_CRITICAL(&s_bridge_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s_bridge.done = done;
    s_bridge.context = context;
    s_bridge.active = true;
    taskEXIT_CRITICAL(&s_bridge_mux);

    device_control_job_t job = {0};
    job.priority = DEVICE_CTRL_PRIORITY_NORMAL;
    job.source = DEVICE_CTRL_SOURCE_SCHEMA;
    job.max_queue_wait_ms = SCHEMA_CTRL_QUEUE_WAIT_MS;
    job.request.origin = DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY;
    strlcpy(job.request.device_id, message->device_id,
            sizeof(job.request.device_id));
    strlcpy(job.request.command, message->command, sizeof(job.request.command));

    esp_err_t err = device_control_scheduler_submit(&job, schema_scheduler_done,
                                                     &s_bridge, NULL);
    if (err != ESP_OK) {
        taskENTER_CRITICAL(&s_bridge_mux);
        memset(&s_bridge, 0, sizeof(s_bridge));
        taskEXIT_CRITICAL(&s_bridge_mux);
    }
    return err;
}

esp_err_t device_schema_control_adapter_init(void)
{
    taskENTER_CRITICAL(&s_bridge_mux);
    if (s_bridge.active) {
        taskEXIT_CRITICAL(&s_bridge_mux);
        return ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&s_bridge_mux);
    device_schema_set_submitter(schema_scheduler_submit);
    return ESP_OK;
}
