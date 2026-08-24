#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "command_dispatcher.h"
#include "ble_central.h"
#include "device_store.h"
#include "log_buffer.h"

static const char *TAG = "dispatcher";

typedef struct {
    char command_name[GW_MSG_COMMAND_LEN];
    gateway_command_fn_t fn;
} registry_entry_t;

static registry_entry_t s_registry[DISPATCHER_MAX_COMMANDS];
static int s_registry_count = 0;

static void cmd_add_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        result->success = 0;
        snprintf(result->message, sizeof(result->message), "Missing device_id");
        return;
    }
    int rc = device_store_add(msg->device_id, msg->device_id, "generic");
    result->success = (rc == 0);
    snprintf(result->message, sizeof(result->message),
             rc == 0 ? "Device %s added" : "Failed to add device %s", msg->device_id);
}

static void cmd_delete_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        result->success = 0;
        snprintf(result->message, sizeof(result->message), "Missing device_id");
        return;
    }
    int rc = device_store_delete(msg->device_id);
    result->success = (rc == 0);
    snprintf(result->message, sizeof(result->message),
             rc == 0 ? "Device %s deleted" : "Device %s not found", msg->device_id);
}

static void cmd_edit_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        result->success = 0;
        snprintf(result->message, sizeof(result->message), "Missing device_id");
        return;
    }
    int rc = device_store_edit(msg->device_id, NULL, NULL);
    result->success = (rc == 0);
    snprintf(result->message, sizeof(result->message),
             rc == 0 ? "Device %s edited" : "Device %s not found", msg->device_id);
}

static void cmd_list_devices(const gw_message_t *msg, dispatch_result_t *result)
{
    int count = 0;
    const device_entry_t *list = device_store_list(&count);

    int offset = snprintf(result->message, sizeof(result->message), "[");
    for (int i = 0; i < count && offset < (int)sizeof(result->message) - 1; i++) {
        offset += snprintf(result->message + offset, sizeof(result->message) - offset,
                            "%s{\"device_id\":\"%s\",\"name\":\"%s\",\"connected\":%d}",
                            (i > 0) ? "," : "", list[i].device_id, list[i].name, list[i].connected);
    }
    snprintf(result->message + offset, sizeof(result->message) - offset, "]");
    result->success = 1;
}

static void cmd_get_status(const gw_message_t *msg, dispatch_result_t *result)
{
    int count = 0;
    device_store_list(&count);
    snprintf(result->message, sizeof(result->message),
             "{\"status\":\"ok\",\"device_count\":%d}", count);
    result->success = 1;
}

static void handle_device_command(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        result->success = 0;
        snprintf(result->message, sizeof(result->message), "Missing device_id");
        return;
    }

    if (!ble_central_is_connected(msg->device_id)) {
        result->success = 0;
        snprintf(result->message, sizeof(result->message), "Device %s is not connected", msg->device_id);
        return;
    }

    int rc = ble_central_send_command(msg->device_id, msg);
    result->success = (rc == 0);
    if (rc == 0) {
        snprintf(result->message, sizeof(result->message), "Command '%s' sent to %s", msg->command, msg->device_id);
    } else {
        snprintf(result->message, sizeof(result->message), "Failed to send command to %s", msg->device_id);
    }

    char log_line[LOG_ENTRY_MAX_LEN];
    snprintf(log_line, sizeof(log_line), "[SENT] device=%s command=%s success=%d",
              msg->device_id, msg->command, result->success);
    log_buffer_push(log_line);
}

static void handle_gateway_command(const gw_message_t *msg, dispatch_result_t *result)
{
    for (int i = 0; i < s_registry_count; i++) {
        if (strcmp(s_registry[i].command_name, msg->command) == 0) {
            s_registry[i].fn(msg, result);
            return;
        }
    }
    result->success = 0;
    snprintf(result->message, sizeof(result->message), "Unknown gateway command: %s", msg->command);
}

int command_dispatcher_register(const char *command_name, gateway_command_fn_t fn)
{
    if (s_registry_count >= DISPATCHER_MAX_COMMANDS) {
        ESP_LOGE(TAG, "Registry full (max=%d)", DISPATCHER_MAX_COMMANDS);
        return -1;
    }
    strncpy(s_registry[s_registry_count].command_name, command_name, GW_MSG_COMMAND_LEN - 1);
    s_registry[s_registry_count].fn = fn;
    s_registry_count++;
    ESP_LOGI(TAG, "Registered gateway command: %s", command_name);
    return 0;
}

int command_dispatcher_init(void)
{
    s_registry_count = 0;
    command_dispatcher_register("add_device", cmd_add_device);
    command_dispatcher_register("delete_device", cmd_delete_device);
    command_dispatcher_register("edit_device", cmd_edit_device);
    command_dispatcher_register("list_devices", cmd_list_devices);
    command_dispatcher_register("get_status", cmd_get_status);
    ESP_LOGI(TAG, "Command dispatcher initialized with %d default commands", s_registry_count);
    return 0;
}

void command_dispatcher_handle(const gw_message_t *msg, dispatch_result_t *result)
{
    memset(result, 0, sizeof(dispatch_result_t));

    if (msg == NULL) {
        result->success = 0;
        snprintf(result->message, sizeof(result->message), "Null message");
        return;
    }

    if (strcmp(msg->type, "device_command") == 0) {
        handle_device_command(msg, result);
    } else if (strcmp(msg->type, "gateway_command") == 0) {
        handle_gateway_command(msg, result);
    } else {
        result->success = 0;
        snprintf(result->message, sizeof(result->message), "Unknown message type: %s", msg->type);
    }
}

void command_dispatcher_on_device_notify(const char *device_id, const gw_message_t *msg)
{
    char log_line[LOG_ENTRY_MAX_LEN];
    snprintf(log_line, sizeof(log_line), "[NOTIFY] device=%s command=%s int_value=%d",
             device_id, msg->command, msg->int_value);
    log_buffer_push(log_line);
    ESP_LOGI(TAG, "%s", log_line);
}
