#include "device_command_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "memory_policy.h"
#include "device_schema.h"

static const char *TAG = "dev_cmd_svc";

/* ── Configuration ───────────────────────────────────────────────────── */

#define SVC_QUEUE_LEN        8
#define SVC_TASK_STACK      3072
#define SVC_TASK_PRIORITY   (tskIDLE_PRIORITY + 4)
#define MAX_PENDING         4
#define ACK_TIMEOUT_MS      2000
#define DEINIT_WAIT_BUDGET_MS 5000

/* ── Event types ─────────────────────────────────────────────────────── */

typedef enum {
    SVC_EVENT_SUBMIT = 0,
    SVC_EVENT_ACK,
    SVC_EVENT_DISCONNECT,
    SVC_EVENT_SHUTDOWN,
} svc_event_type_t;

typedef struct {
    svc_event_type_t type;

    /* SUBMIT event data */
    device_command_request_t request;
    device_command_completion_fn completion;
    void *context;

    /* ACK event data */
    char ack_device_id[GW_MSG_DEVICE_ID_LEN];
    gw_message_t ack_message;

    /* DISCONNECT event data */
    char disconnect_device_id[GW_MSG_DEVICE_ID_LEN];
} svc_event_t;

/* ── Pending slot ────────────────────────────────────────────────────── */

typedef struct {
    bool in_use;
    uint32_t request_id;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    device_command_origin_t origin;
    device_command_completion_fn completion;
    void *context;
    int64_t deadline_us;

    /* Cached typed values for result */
    bool has_bool_value;
    bool bool_value;
    bool has_int_value;
    int32_t int_value;
    bool has_feature_id;
    char feature_id[GW_FEATURE_ID_LEN];
    bool has_property_id;
    uint8_t property_id;
} pending_slot_t;

/* ── Service state ───────────────────────────────────────────────────── */

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_running;
static pending_slot_t s_pending[MAX_PENDING];
static uint32_t s_next_request_id;
static device_command_service_stats_t s_stats;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── Transport hooks ─────────────────────────────────────────────────── */

static int default_send_command(const char *device_id,
                                const gw_message_t *message);
static int default_is_connected(const char *device_id);

static device_command_transport_hooks_t s_hooks = {
    .send_command = default_send_command,
    .is_connected = default_is_connected,
};

static void build_wire_message(const device_command_request_t *request,
                               uint32_t request_id, gw_message_t *msg);

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void stats_inc(uint32_t *field)
{
    taskENTER_CRITICAL(&s_stats_mux);
    (*field)++;
    taskEXIT_CRITICAL(&s_stats_mux);
}

static uint32_t generate_request_id(void)
{
    uint32_t id;
    bool collision;
    do {
        id = ++s_next_request_id;
        if (id == 0) {
            id = ++s_next_request_id;
        }
        collision = false;
        for (size_t i = 0; i < MAX_PENDING; i++) {
            if (s_pending[i].in_use && s_pending[i].request_id == id) {
                collision = true;
                break;
            }
        }
    } while (collision);
    return id;
}

static pending_slot_t *find_pending_by_device(const char *device_id)
{
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (s_pending[i].in_use &&
            strcmp(s_pending[i].device_id, device_id) == 0) {
            return &s_pending[i];
        }
    }
    return NULL;
}

static pending_slot_t *find_pending_by_id(const char *device_id,
                                           uint32_t request_id)
{
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (s_pending[i].in_use &&
            strcmp(s_pending[i].device_id, device_id) == 0 &&
            s_pending[i].request_id == request_id) {
            return &s_pending[i];
        }
    }
    return NULL;
}

static pending_slot_t *allocate_slot(void)
{
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (!s_pending[i].in_use) {
            return &s_pending[i];
        }
    }
    return NULL;
}

static void complete_slot(pending_slot_t *slot,
                          const device_command_result_t *result)
{
    if (slot->completion != NULL) {
        slot->completion(result, slot->context);
    }
    slot->in_use = false;
    slot->completion = NULL;
    slot->context = NULL;
}

static void complete_slot_status(pending_slot_t *slot,
                                  device_command_status_t status)
{
    device_command_result_t result = {0};
    result.status = status;
    result.request_id = slot->request_id;
    complete_slot(slot, &result);
}

/* ── Origin validation ───────────────────────────────────────────────── */

static device_command_status_t validate_request(
    const device_command_request_t *request)
{
    if (request->device_id[0] == '\0') {
        return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
    }

    switch (request->origin) {
    case DEVICE_CMD_ORIGIN_CONTROL:
        if (request->command[0] == '\0') {
            return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
        }
        {
            gw_message_t message;
            build_wire_message(request, 0, &message);
            message.has_request_id = 0;
            device_schema_validation_t validation =
                device_schema_validate_command(&message, NULL);
            if (validation == DEVICE_SCHEMA_VALID_UNSUPPORTED_COMMAND) {
                return DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND;
            }
            if (validation == DEVICE_SCHEMA_VALID_ARGUMENT) {
                return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
            }
            if (validation != DEVICE_SCHEMA_VALID) {
                return DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND;
            }
        }
        break;

    case DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY:
        if (strcmp(request->command, "describe_capabilities") != 0) {
            return DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND;
        }
        break;

    case DEVICE_CMD_ORIGIN_STATE_READ:
        if (strcmp(request->command, "read_feature_state") != 0) {
            return DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND;
        }
        if (!request->has_feature_id || request->feature_id[0] == '\0') {
            return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
        }
        if (!request->has_property_id) {
            return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
        }
        break;
    }

    return DEVICE_CMD_STATUS_OK;
}

/* ── Wire message builder ────────────────────────────────────────────── */

static void build_wire_message(const device_command_request_t *request,
                                uint32_t request_id,
                                gw_message_t *msg)
{
    memset(msg, 0, sizeof(*msg));
    msg->protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(msg->type, "device_command", sizeof(msg->type));
    strlcpy(msg->device_id, request->device_id, sizeof(msg->device_id));
    strlcpy(msg->command, request->command, sizeof(msg->command));
    msg->has_device_id = 1;
    msg->request_id = request_id;
    msg->has_request_id = 1;

    if (request->has_bool_value) {
        msg->bool_value = request->bool_value ? 1 : 0;
        msg->has_bool_value = 1;
    }
    if (request->has_int_value) {
        msg->int_value = request->int_value;
        msg->has_int_value = 1;
    }
    if (request->has_feature_id) {
        strlcpy(msg->feature_id, request->feature_id, sizeof(msg->feature_id));
        msg->has_feature_id = 1;
    }
    if (request->has_property_id) {
        msg->property_id = request->property_id;
        msg->has_property_id = 1;
    }
}

/* ── Submit handling ─────────────────────────────────────────────────── */

static void handle_submit(const svc_event_t *event)
{
    const device_command_request_t *request = &event->request;

    /* Validate by origin */
    device_command_status_t validation = validate_request(request);
    if (validation != DEVICE_CMD_STATUS_OK) {
        device_command_result_t result = {0};
        result.status = validation;
        if (event->completion != NULL) {
            event->completion(&result, event->context);
        }
        return;
    }

    /* Check connection for CONTROL origin */
    if (request->origin == DEVICE_CMD_ORIGIN_CONTROL) {
        if (s_hooks.is_connected(request->device_id) <= 0) {
            device_command_result_t result = {0};
            result.status = DEVICE_CMD_STATUS_NOT_CONNECTED;
            if (event->completion != NULL) {
                event->completion(&result, event->context);
            }
            return;
        }
    }

    /* One pending per device */
    if (find_pending_by_device(request->device_id) != NULL) {
        stats_inc(&s_stats.busy_rejections);
        device_command_result_t result = {0};
        result.status = DEVICE_CMD_STATUS_BUSY;
        if (event->completion != NULL) {
            event->completion(&result, event->context);
        }
        return;
    }

    /* Allocate slot */
    pending_slot_t *slot = allocate_slot();
    if (slot == NULL) {
        stats_inc(&s_stats.queue_full);
        device_command_result_t result = {0};
        result.status = DEVICE_CMD_STATUS_INTERNAL_ERROR;
        if (event->completion != NULL) {
            event->completion(&result, event->context);
        }
        return;
    }

    /* Fill slot */
    slot->in_use = true;
    slot->request_id = generate_request_id();
    strlcpy(slot->device_id, request->device_id, sizeof(slot->device_id));
    strlcpy(slot->command, request->command, sizeof(slot->command));
    slot->origin = request->origin;
    slot->completion = event->completion;
    slot->context = event->context;
    slot->deadline_us = esp_timer_get_time() + ACK_TIMEOUT_MS * 1000LL;
    slot->has_bool_value = request->has_bool_value;
    slot->bool_value = request->bool_value;
    slot->has_int_value = request->has_int_value;
    slot->int_value = request->int_value;
    slot->has_feature_id = request->has_feature_id;
    if (request->has_feature_id) {
        strlcpy(slot->feature_id, request->feature_id,
                sizeof(slot->feature_id));
    }
    slot->has_property_id = request->has_property_id;
    slot->property_id = request->property_id;

    /* Update max pending stats */
    uint32_t count = 0;
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (s_pending[i].in_use) count++;
    }
    taskENTER_CRITICAL(&s_stats_mux);
    if (count > s_stats.max_pending) {
        s_stats.max_pending = count;
    }
    taskEXIT_CRITICAL(&s_stats_mux);

    /* Build wire message and send */
    gw_message_t wire_msg;
    build_wire_message(request, slot->request_id, &wire_msg);

    ESP_LOGI(TAG, "[SEND] device=%s request_id=%lu command=%s origin=%d",
             slot->device_id, (unsigned long)slot->request_id,
             slot->command, slot->origin);

    int send_rc = s_hooks.send_command(slot->device_id, &wire_msg);
    if (send_rc != 0) {
        ESP_LOGW(TAG, "[SEND_FAILED] device=%s request_id=%lu",
                 slot->device_id, (unsigned long)slot->request_id);
        stats_inc(&s_stats.transport_errors);
        complete_slot_status(slot, DEVICE_CMD_STATUS_TRANSPORT_ERROR);
        return;
    }

    stats_inc(&s_stats.submitted);
}

/* ── ACK handling ────────────────────────────────────────────────────── */

static void handle_ack(const svc_event_t *event)
{
    const char *device_id = event->ack_device_id;
    const gw_message_t *msg = &event->ack_message;

    if (!msg->has_request_id || msg->request_id == 0) {
        return;
    }

    pending_slot_t *slot = find_pending_by_id(device_id, msg->request_id);
    if (slot == NULL) {
        ESP_LOGI(TAG, "[ACK_UNMATCHED] device=%s request_id=%lu",
                 device_id, (unsigned long)msg->request_id);
        return;
    }

    /* Verify command matches */
    if (strcmp(slot->command, msg->command) != 0) {
        ESP_LOGW(TAG, "[ACK_CMD_MISMATCH] device=%s request_id=%lu "
                 "expected=%s got=%s",
                 device_id, (unsigned long)msg->request_id,
                 slot->command, msg->command);
        return;
    }

    ESP_LOGI(TAG, "[ACK] device=%s request_id=%lu command=%s accepted=%d",
             device_id, (unsigned long)msg->request_id,
             slot->command, msg->bool_value);

    /* Build typed result */
    device_command_result_t result = {0};
    result.request_id = slot->request_id;

    if (msg->bool_value) {
        result.status = DEVICE_CMD_STATUS_OK;
        result.accepted = true;

        if (msg->has_bool_value) {
            result.has_bool_value = true;
            result.bool_value = msg->bool_value != 0;
        }
        if (msg->has_int_value) {
            result.has_int_value = true;
            result.int_value = msg->int_value;
        }
        if (msg->has_feature_value_bool) {
            result.has_feature_value_bool = true;
            result.feature_value_bool = msg->feature_value_bool;
        }
        if (msg->has_feature_value_int) {
            result.has_feature_value_int = true;
            result.feature_value_int = msg->feature_value_int;
        }

        stats_inc(&s_stats.completed_ok);
    } else {
        result.status = DEVICE_CMD_STATUS_DEVICE_REJECTED;
        result.accepted = false;
        stats_inc(&s_stats.completed_error);
    }

    complete_slot(slot, &result);
}

/* ── Disconnect handling ─────────────────────────────────────────────── */

static void handle_disconnect(const svc_event_t *event)
{
    const char *device_id = event->disconnect_device_id;
    pending_slot_t *slot = find_pending_by_device(device_id);
    if (slot != NULL) {
        ESP_LOGI(TAG, "[DISCONNECT] device=%s request_id=%lu",
                 device_id, (unsigned long)slot->request_id);
        stats_inc(&s_stats.disconnect_count);
        complete_slot_status(slot, DEVICE_CMD_STATUS_NOT_CONNECTED);
    }
}

/* ── Timeout check ───────────────────────────────────────────────────── */

static void check_timeouts(void)
{
    int64_t now_us = esp_timer_get_time();

    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (s_pending[i].in_use && now_us >= s_pending[i].deadline_us) {
            ESP_LOGW(TAG, "[TIMEOUT] device=%s request_id=%lu",
                     s_pending[i].device_id,
                     (unsigned long)s_pending[i].request_id);
            stats_inc(&s_stats.timeout_count);
            complete_slot_status(&s_pending[i], DEVICE_CMD_STATUS_TIMEOUT);
        }
    }
}

/* ── Service task ────────────────────────────────────────────────────── */

static void service_task(void *arg)
{
    (void)arg;
    svc_event_t event;

    while (s_running) {
        /* Wait for event or nearest deadline */
        int64_t now_us = esp_timer_get_time();
        int64_t nearest_deadline_us = now_us + 1000000LL; /* 1s default */

        for (size_t i = 0; i < MAX_PENDING; i++) {
            if (s_pending[i].in_use && s_pending[i].deadline_us < nearest_deadline_us) {
                nearest_deadline_us = s_pending[i].deadline_us;
            }
        }

        int64_t wait_us = nearest_deadline_us - esp_timer_get_time();
        TickType_t wait_ticks = (wait_us > 0)
            ? pdMS_TO_TICKS(wait_us / 1000)
            : 0;

        if (xQueueReceive(s_queue, &event, wait_ticks) == pdTRUE) {
            switch (event.type) {
            case SVC_EVENT_SUBMIT:
                handle_submit(&event);
                break;
            case SVC_EVENT_ACK:
                handle_ack(&event);
                break;
            case SVC_EVENT_DISCONNECT:
                handle_disconnect(&event);
                break;
            case SVC_EVENT_SHUTDOWN:
                s_running = false;
                break;
            }
        }

        /* Check timeouts after each iteration */
        check_timeouts();
    }

    /* Complete any remaining pending requests */
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (s_pending[i].in_use) {
            complete_slot_status(&s_pending[i], DEVICE_CMD_STATUS_INTERNAL_ERROR);
        }
    }

    ESP_LOGI(TAG, "Service task stopped");
    vTaskDelete(NULL);
}

/* ── Default transport hooks (BLE central) ───────────────────────────── */

static int default_send_command(const char *device_id,
                                const gw_message_t *message)
{
    /* Lazy reference to avoid circular dependency at compile time.
     * In production, ble_central_send_command is linked. */
    extern int ble_central_send_command(const char *device_id,
                                        const gw_message_t *message);
    return ble_central_send_command(device_id, message);
}

static int default_is_connected(const char *device_id)
{
    extern int ble_central_is_connected(const char *device_id);
    return ble_central_is_connected(device_id);
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t device_command_service_init(void)
{
    if (s_queue != NULL) return ESP_ERR_INVALID_STATE;

    memset(&s_pending, 0, sizeof(s_pending));
    memset(&s_stats, 0, sizeof(s_stats));
    s_next_request_id = 0;

    s_queue = xQueueCreate(SVC_QUEUE_LEN, sizeof(svc_event_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;

    s_running = true;
    BaseType_t created = xTaskCreate(service_task, "dev_cmd_svc",
                                     SVC_TASK_STACK, NULL,
                                     SVC_TASK_PRIORITY, &s_task);
    if (created != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Device command service started (queue=%d, pending=%d)",
             SVC_QUEUE_LEN, MAX_PENDING);
    return ESP_OK;
}

void device_command_service_deinit(void)
{
    if (s_queue == NULL) return;

    svc_event_t shutdown = { .type = SVC_EVENT_SHUTDOWN };
    xQueueSend(s_queue, &shutdown, pdMS_TO_TICKS(100));

    /* Wait for task to stop */
    for (int wait_ms = 0; wait_ms < DEINIT_WAIT_BUDGET_MS && s_running; wait_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    vQueueDelete(s_queue);
    s_queue = NULL;
    s_task = NULL;

    ESP_LOGI(TAG, "Device command service stopped");
}

esp_err_t device_command_service_submit(
    const device_command_request_t *request,
    device_command_completion_fn completion,
    void *context)
{
    if (request == NULL || completion == NULL) return ESP_ERR_INVALID_ARG;
    if (s_queue == NULL || !s_running) return ESP_ERR_INVALID_STATE;

    svc_event_t event = {0};
    event.type = SVC_EVENT_SUBMIT;
    event.request = *request;
    event.completion = completion;
    event.context = context;

    if (xQueueSend(s_queue, &event, 0) != pdTRUE) {
        stats_inc(&s_stats.queue_full);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool device_command_service_on_notify(
    const char *device_id,
    const gw_message_t *message)
{
    if (device_id == NULL || message == NULL) return false;
    if (strcmp(message->type, "device_ack") != 0) return false;
    if (!message->has_request_id || message->request_id == 0) return false;

    svc_event_t event = {0};
    event.type = SVC_EVENT_ACK;
    strlcpy(event.ack_device_id, device_id, sizeof(event.ack_device_id));
    event.ack_message = *message;

    if (xQueueSend(s_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ACK event queue full for device=%s", device_id);
        return false;
    }

    return true;
}

void device_command_service_on_disconnect(const char *device_id)
{
    if (device_id == NULL) return;

    svc_event_t event = {0};
    event.type = SVC_EVENT_DISCONNECT;
    strlcpy(event.disconnect_device_id, device_id,
            sizeof(event.disconnect_device_id));

    /* Best-effort: if queue is full, pending slots will be cleaned up
     * on timeout anyway. */
    xQueueSend(s_queue, &event, pdMS_TO_TICKS(100));
}

void device_command_service_get_stats(device_command_service_stats_t *out)
{
    if (out == NULL) return;
    taskENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    taskEXIT_CRITICAL(&s_stats_mux);
}

void device_command_service_set_hooks(
    const device_command_transport_hooks_t *hooks)
{
    if (hooks == NULL) {
        s_hooks.send_command = default_send_command;
        s_hooks.is_connected = default_is_connected;
    } else {
        s_hooks = *hooks;
    }
}

uint32_t device_command_service_get_pending_count(void)
{
    uint32_t count = 0;
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (s_pending[i].in_use) count++;
    }
    return count;
}
