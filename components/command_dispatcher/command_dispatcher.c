#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "command_dispatcher.h"
#include "command_dispatcher_internal.h"
#include "device_request_manager.h"

static const char *TAG = "dispatcher";

static bool s_initialized;
static bool s_registry_frozen;

void command_dispatcher_set_text_result(dispatch_result_t *result,
                                        dispatch_status_t status,
                                        const char *format, ...)
{
    if (result == NULL) return;
    result->status = status;
    result->format = DISPATCH_RESULT_TEXT;
    va_list args;
    va_start(args, format);
    vsnprintf(result->payload, sizeof(result->payload), format, args);
    va_end(args);
}

void command_dispatcher_set_json_result(dispatch_result_t *result,
                                        dispatch_status_t status,
                                        const char *json)
{
    if (result == NULL) return;
    result->status = status;
    result->format = DISPATCH_RESULT_JSON;
    strlcpy(result->payload, json != NULL ? json : "null",
            sizeof(result->payload));
}

int command_dispatcher_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Command dispatcher already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (command_registry_init() != 0 || device_request_manager_init() != 0 ||
        gateway_commands_register_defaults() != 0) {
        return -1;
    }

    s_initialized = true;
    s_registry_frozen = false;
    ESP_LOGI(TAG, "Command dispatcher initialized with %d commands",
             command_registry_count());
    return ESP_OK;
}

int command_dispatcher_freeze_registry(void)
{
    if (!s_initialized) return -1;
    if (!s_registry_frozen && command_registry_freeze() != 0) return -1;
    s_registry_frozen = true;
    return 0;
}

int command_dispatcher_register(const char *command_name, gateway_command_fn_t fn)
{
    return command_registry_register(command_name, fn);
}

bool command_dispatcher_is_registered(const char *command_name)
{
    return command_registry_find(command_name) != NULL;
}

int command_dispatcher_get_registered_names(
    char out_names[][GW_MSG_COMMAND_LEN], int max_names)
{
    return command_registry_get_names(out_names, max_names);
}

// Command ingress validation (refactor plan §15.1).
static bool validate_command_message(const gw_message_t *msg)
{
    if (msg->protocol_version != GW_PROTOCOL_VERSION) {
        return false;
    }
    size_t type_len = strnlen(msg->type, sizeof(msg->type));
    if (type_len == 0 || type_len >= sizeof(msg->type)) return false;

    if (strcmp(msg->type, "gateway_command") == 0) {
        return msg->command[0] != '\0';
    }
    if (strcmp(msg->type, "device_command") == 0) {
        return msg->has_device_id && msg->device_id[0] != '\0' &&
               msg->command[0] != '\0';
    }
    return true;
}

void command_dispatcher_handle(const gw_message_t *msg, dispatch_result_t *result)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));

    if (!s_initialized || !s_registry_frozen) {
        command_dispatcher_set_text_result(result, DISPATCH_STATUS_INTERNAL_ERROR,
                                           "Command dispatcher is not ready");
        return;
    }
    if (msg == NULL) {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INVALID_ARGUMENT,
                                           "Null message");
        return;
    }
    if (!validate_command_message(msg)) {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INVALID_ARGUMENT,
                                           "Invalid message");
        return;
    }

    if (strcmp(msg->type, "device_command") == 0) {
        device_command_handle(msg, result);
    } else if (strcmp(msg->type, "gateway_command") == 0) {
        gateway_command_handle(msg, result);
    } else {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_NOT_FOUND,
                                           "Unknown message type: %s", msg->type);
    }
}

void command_dispatcher_on_device_notify(const char *device_id,
                                         const gw_message_t *msg)
{
    // Notification ingress validation (refactor plan §15.2).
    if (device_id == NULL || device_id[0] == '\0' || msg == NULL ||
        msg->protocol_version != GW_PROTOCOL_VERSION) {
        ESP_LOGW(TAG, "[NOTIFY_DROPPED] invalid notification arguments");
        return;
    }

    if (strcmp(msg->type, "device_event") == 0) {
        // Events never complete pending commands.
        ESP_LOGD(TAG, "[DEVICE_EVENT] device=%s command=%s value=%d",
                 device_id, msg->command, msg->int_value);
        return;
    }
    if (strcmp(msg->type, "device_ack") != 0) {
        ESP_LOGW(TAG, "[NOTIFY_DROPPED] device=%s unexpected type=%s",
                 device_id, msg->type);
        return;
    }
    if (!msg->has_device_id || msg->device_id[0] == '\0' ||
        !msg->has_request_id || msg->request_id == 0 ||
        msg->command[0] == '\0') {
        ESP_LOGW(TAG,
                 "[ACK_UNMATCHED] device=%s malformed ACK (device_id=%d request_id=%lu command='%s')",
                 device_id, msg->has_device_id,
                 (unsigned long)msg->request_id, msg->command);
        return;
    }

    device_command_on_notify(device_id, msg);
}

void command_dispatcher_reset_for_test(void)
{
    s_initialized = false;
    s_registry_frozen = false;
    command_registry_init();
    device_request_manager_init();
}
