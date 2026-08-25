#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ble_central.h"
#include "command_dispatcher.h"
#include "command_dispatcher_internal.h"
#include "device_store.h"

static const char *TAG = "dispatcher";

#define DISPATCHER_MAX_PENDING_ACKS DEVICE_STORE_MAX_DEVICES

typedef struct {
    bool in_use;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    gw_message_t response;
    SemaphoreHandle_t semaphore;
} pending_ack_t;

static SemaphoreHandle_t s_ack_mutex;
static pending_ack_t s_pending_acks[DISPATCHER_MAX_PENDING_ACKS];

static pending_ack_t *allocate_pending_ack(const gw_message_t *msg)
{
    if (xSemaphoreTake(s_ack_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return NULL;

    pending_ack_t *available = NULL;
    for (int i = 0; i < DISPATCHER_MAX_PENDING_ACKS; i++) {
        if (s_pending_acks[i].in_use &&
            strcmp(s_pending_acks[i].device_id, msg->device_id) == 0) {
            xSemaphoreGive(s_ack_mutex);
            return NULL;
        }
        if (!s_pending_acks[i].in_use && available == NULL) {
            available = &s_pending_acks[i];
        }
    }

    if (available != NULL) {
        while (xSemaphoreTake(available->semaphore, 0) == pdTRUE) {}
        available->in_use = true;
        strlcpy(available->device_id, msg->device_id, sizeof(available->device_id));
        strlcpy(available->command, msg->command, sizeof(available->command));
        memset(&available->response, 0, sizeof(available->response));
    }
    xSemaphoreGive(s_ack_mutex);
    return available;
}

static void release_pending_ack(pending_ack_t *pending)
{
    if (pending == NULL ||
        xSemaphoreTake(s_ack_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    pending->in_use = false;
    pending->device_id[0] = '\0';
    pending->command[0] = '\0';
    xSemaphoreGive(s_ack_mutex);
}

int device_command_init(void)
{
    if (s_ack_mutex == NULL) s_ack_mutex = xSemaphoreCreateMutex();
    if (s_ack_mutex == NULL ||
        xSemaphoreTake(s_ack_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }

    for (int i = 0; i < DISPATCHER_MAX_PENDING_ACKS; i++) {
        if (s_pending_acks[i].semaphore == NULL) {
            s_pending_acks[i].semaphore = xSemaphoreCreateBinary();
        }
        s_pending_acks[i].in_use = false;
        s_pending_acks[i].device_id[0] = '\0';
        s_pending_acks[i].command[0] = '\0';
        if (s_pending_acks[i].semaphore == NULL) {
            xSemaphoreGive(s_ack_mutex);
            return -1;
        }
        while (xSemaphoreTake(s_pending_acks[i].semaphore, 0) == pdTRUE) {}
    }

    xSemaphoreGive(s_ack_mutex);
    return 0;
}

void device_command_handle(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        command_dispatcher_set_result(result, false, "Missing device_id");
        return;
    }
    if (!ble_central_is_connected(msg->device_id)) {
        command_dispatcher_set_result(result, false, "Device %s is not connected",
                                      msg->device_id);
        return;
    }
    if (s_ack_mutex == NULL) {
        command_dispatcher_set_result(result, false, "Command dispatcher is not initialized");
        return;
    }

    pending_ack_t *pending = allocate_pending_ack(msg);
    if (pending == NULL) {
        command_dispatcher_set_result(result, false,
                                      "Another command for %s is pending",
                                      msg->device_id);
        return;
    }

    int send_rc = ble_central_send_command(msg->device_id, msg);
    ESP_LOGI(TAG, "[SENT] device=%s command=%s success=%d", msg->device_id,
             msg->command, send_rc == 0);

    if (send_rc != 0) {
        release_pending_ack(pending);
        command_dispatcher_set_result(result, false, "Could not send command to %s",
                                      msg->device_id);
        return;
    }

    if (xSemaphoreTake(pending->semaphore,
                       pdMS_TO_TICKS(DISPATCHER_ACK_TIMEOUT_MS)) != pdTRUE) {
        release_pending_ack(pending);
        command_dispatcher_set_result(result, false,
                                      "No ACK from %s within %d ms", msg->device_id,
                                      DISPATCHER_ACK_TIMEOUT_MS);
        return;
    }

    gw_message_t response;
    if (xSemaphoreTake(s_ack_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        response = pending->response;
        pending->in_use = false;
        pending->device_id[0] = '\0';
        pending->command[0] = '\0';
        xSemaphoreGive(s_ack_mutex);
    } else {
        release_pending_ack(pending);
        command_dispatcher_set_result(result, false, "Could not read ACK from %s",
                                      msg->device_id);
        return;
    }

    command_dispatcher_set_result(
        result, response.bool_value != 0,
        response.bool_value ? "Device %s acknowledged '%s'"
                            : "Device %s rejected '%s'",
        msg->device_id, msg->command);
}

void device_command_on_notify(const char *device_id, const gw_message_t *msg)
{
    if (device_id == NULL || msg == NULL) return;

    bool matched = false;
    if (s_ack_mutex != NULL &&
        xSemaphoreTake(s_ack_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        for (int i = 0; i < DISPATCHER_MAX_PENDING_ACKS; i++) {
            pending_ack_t *pending = &s_pending_acks[i];
            if (pending->in_use && strcmp(pending->device_id, device_id) == 0 &&
                (msg->command[0] == '\0' ||
                 strcmp(pending->command, msg->command) == 0)) {
                pending->response = *msg;
                xSemaphoreGive(pending->semaphore);
                matched = true;
                break;
            }
        }
        xSemaphoreGive(s_ack_mutex);
    }

    ESP_LOGI(TAG, "%s device=%s command=%s success=%d value=%d",
             matched ? "[ACK]" : "[NOTIFY]", device_id, msg->command,
             msg->bool_value != 0, msg->int_value);
}
