#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "command_dispatcher.h"
#include "command_dispatcher_internal.h"

static const char *TAG = "dispatcher";

void command_dispatcher_set_result(dispatch_result_t *result, bool success,
                                   const char *format, ...)
{
    result->success = success;
    va_list args;
    va_start(args, format);
    vsnprintf(result->message, sizeof(result->message), format, args);
    va_end(args);
}

int command_dispatcher_init(void)
{
    if (command_registry_init() != 0 || device_command_init() != 0 ||
        gateway_commands_register_defaults() != 0) {
        return -1;
    }

    ESP_LOGI(TAG, "Command dispatcher initialized with %d commands",
             command_registry_count());
    return 0;
}

void command_dispatcher_handle(const gw_message_t *msg, dispatch_result_t *result)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));

    if (msg == NULL) {
        command_dispatcher_set_result(result, false, "Null message");
        return;
    }

    if (strcmp(msg->type, "device_command") == 0) {
        device_command_handle(msg, result);
    } else if (strcmp(msg->type, "gateway_command") == 0) {
        gateway_command_handle(msg, result);
    } else {
        command_dispatcher_set_result(result, false, "Unknown message type: %s",
                                      msg->type);
    }
}

void command_dispatcher_on_device_notify(const char *device_id,
                                         const gw_message_t *msg)
{
    device_command_on_notify(device_id, msg);
}
