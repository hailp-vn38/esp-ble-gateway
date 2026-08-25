#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "ble_central.h"
#include "command_dispatcher.h"
#include "command_dispatcher_internal.h"
#include "device_request_manager.h"

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
    device_request_release(pending);

    ESP_LOGI(TAG, "[CMD_ACK] device=%s request_id=%lu command=%s result=%s",
             msg->device_id, (unsigned long)response.request_id,
             msg->command, accepted ? "ok" : "rejected");

    command_dispatcher_set_text_result(
        result,
        accepted ? DISPATCH_STATUS_OK : DISPATCH_STATUS_DEVICE_ERROR,
        accepted ? "Device %s acknowledged '%s'" : "Device %s rejected '%s'",
        msg->device_id, msg->command);
}

void device_command_on_notify(const char *device_id, const gw_message_t *msg)
{
    device_request_complete(device_id, msg);
}
