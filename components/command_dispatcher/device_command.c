#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "ble_central.h"
#include "command_dispatcher.h"
#include "command_dispatcher_internal.h"
#include "device_request_manager.h"
#include "device_schema.h"

static const char *TAG = "dispatcher";

static int ble_send_command_hook(const char *device_id, const gw_message_t *msg)
{
    return ble_central_send_command(device_id, msg);
}

static int ble_is_connected_hook(const char *device_id)
{
    return ble_central_is_connected(device_id);
}

static device_command_hooks_t s_hooks = {
    .send_command = ble_send_command_hook,
    .is_connected = ble_is_connected_hook,
};

void device_command_set_hooks(const device_command_hooks_t *hooks)
{
    if (hooks == NULL) {
        s_hooks.send_command = ble_send_command_hook;
        s_hooks.is_connected = ble_is_connected_hook;
    } else {
        s_hooks = *hooks;
    }
}

int device_command_init(void)
{
    return device_request_manager_init();
}

void device_command_handle(const gw_message_t *msg, dispatch_result_t *result)
{
    // Boundary validation for device_id/command presence already ran in
    // command_dispatcher_handle(); only transport readiness is checked here.
    device_schema_validation_t validation =
        device_schema_validate_command(msg, NULL);
    if (validation == DEVICE_SCHEMA_VALID_UNSUPPORTED_COMMAND) {
        command_dispatcher_set_text_result(
            result, DISPATCH_STATUS_UNSUPPORTED_COMMAND,
            "Device %s does not advertise command '%s'", msg->device_id,
            msg->command);
        return;
    }
    if (validation == DEVICE_SCHEMA_VALID_ARGUMENT) {
        command_dispatcher_set_text_result(
            result, DISPATCH_STATUS_INVALID_COMMAND_ARGUMENT,
            "Invalid argument for command '%s'", msg->command);
        return;
    }

    if (s_hooks.is_connected(msg->device_id) <= 0) {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_NOT_CONNECTED,
                                           "Device %s is not connected",
                                           msg->device_id);
        return;
    }

    pending_request_t *pending = NULL;
    int allocate_rc = device_request_allocate(msg->device_id, msg->command,
                                              &pending);
    if (allocate_rc != 0) {
        command_dispatcher_set_text_result(result, DISPATCH_STATUS_BUSY,
                                           "Another command for %s is pending",
                                           msg->device_id);
        return;
    }

    // The caller's message is immutable: correlation metadata belongs to the
    // dispatcher, so a local wire copy carries the freshly assigned request_id.
    gw_message_t wire_msg = *msg;
    // Strict v4: every outbound command advertises the current protocol.
    wire_msg.protocol_version = GW_PROTOCOL_VERSION;
    wire_msg.request_id = pending->request_id;
    wire_msg.has_request_id = 1;

    ESP_LOGI(TAG, "[CMD_SEND] device=%s request_id=%lu command=%s",
             wire_msg.device_id, (unsigned long)wire_msg.request_id,
             wire_msg.command);

    int send_rc = s_hooks.send_command(wire_msg.device_id, &wire_msg);
    if (send_rc != 0) {
        ESP_LOGW(TAG, "[CMD_SEND_FAILED] device=%s request_id=%lu command=%s",
                 wire_msg.device_id, (unsigned long)wire_msg.request_id,
                 wire_msg.command);
        device_request_release(pending);
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_TRANSPORT_ERROR,
                                           "Could not send command to %s",
                                           msg->device_id);
        return;
    }

    if (device_request_wait(pending,
                            pdMS_TO_TICKS(DISPATCHER_ACK_TIMEOUT_MS)) != 0) {
        ESP_LOGW(TAG, "[CMD_TIMEOUT] device=%s request_id=%lu command=%s timeout_ms=%d",
                 msg->device_id, (unsigned long)pending->request_id,
                 msg->command, DISPATCHER_ACK_TIMEOUT_MS);
        device_request_release(pending);
        command_dispatcher_set_text_result(result, DISPATCH_STATUS_TIMEOUT,
                                           "No ACK from %s within %d ms",
                                           msg->device_id,
                                           DISPATCHER_ACK_TIMEOUT_MS);
        return;
    }

    gw_message_t response = pending->response;
    bool accepted = response.bool_value != 0;
    uint32_t resp_request_id = response.request_id;
    bool resp_has_int = response.has_int_value;
    int32_t resp_int = response.int_value;
    bool resp_has_bool = response.has_bool_value;
    bool resp_bool = response.bool_value != 0;
    device_request_release(pending);

    ESP_LOGI(TAG, "[CMD_ACK] device=%s request_id=%lu command=%s result=%s",
             msg->device_id, (unsigned long)resp_request_id,
             msg->command, accepted ? "ok" : "rejected");

    if (accepted) {
        cJSON *json = cJSON_CreateObject();
        if (json != NULL) {
            cJSON_AddStringToObject(json, "device_id", msg->device_id);
            cJSON_AddStringToObject(json, "command", msg->command);
            cJSON_AddNumberToObject(json, "request_id", resp_request_id);
            cJSON_AddBoolToObject(json, "success", true);
            cJSON *resp = cJSON_AddObjectToObject(json, "response");
            if (resp != NULL) {
                if (resp_has_int) {
                    cJSON_AddBoolToObject(resp, "has_int_value", true);
                    cJSON_AddNumberToObject(resp, "int_value", resp_int);
                }
                if (resp_has_bool) {
                    cJSON_AddBoolToObject(resp, "has_bool_value", true);
                    cJSON_AddBoolToObject(resp, "bool_value", resp_bool);
                }
            }
            char *printed = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            if (printed != NULL) {
                command_dispatcher_set_json_result(result, DISPATCH_STATUS_OK,
                                                   printed);
                free(printed);
                return;
            }
        }
        command_dispatcher_set_text_result(
            result, DISPATCH_STATUS_OK,
            "Device %s acknowledged '%s'", msg->device_id, msg->command);
    } else {
        cJSON *json = cJSON_CreateObject();
        if (json != NULL) {
            cJSON_AddStringToObject(json, "device_id", msg->device_id);
            cJSON_AddStringToObject(json, "command", msg->command);
            cJSON_AddNumberToObject(json, "request_id", resp_request_id);
            cJSON_AddBoolToObject(json, "success", false);
            cJSON *resp = cJSON_AddObjectToObject(json, "response");
            if (resp != NULL) {
                if (resp_has_int) {
                    cJSON_AddBoolToObject(resp, "has_int_value", true);
                    cJSON_AddNumberToObject(resp, "int_value", resp_int);
                }
                if (resp_has_bool) {
                    cJSON_AddBoolToObject(resp, "has_bool_value", true);
                    cJSON_AddBoolToObject(resp, "bool_value", resp_bool);
                }
            }
            char *printed = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            if (printed != NULL) {
                command_dispatcher_set_json_result(
                    result, DISPATCH_STATUS_DEVICE_ERROR, printed);
                free(printed);
                return;
            }
        }
        command_dispatcher_set_text_result(
            result, DISPATCH_STATUS_DEVICE_ERROR,
            "Device %s rejected '%s'", msg->device_id, msg->command);
    }
}

void device_command_on_notify(const char *device_id, const gw_message_t *msg)
{
    device_request_complete(device_id, msg);
}
