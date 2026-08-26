#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "ble_central.h"
#include "gateway_status.h"

#include "command_dispatcher.h"
#include "command_dispatcher_internal.h"
#include "device_store.h"

static const char *TAG = "dispatcher";

static void format_ble_addr(const uint8_t address[6], char output[18])
{
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[5], address[4], address[3], address[2], address[1], address[0]);
}

static void cmd_add_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id || msg->device_id[0] == '\0') {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INVALID_ARGUMENT,
                                           "Missing device_id");
        return;
    }

    const char *name = msg->name[0] != '\0' ? msg->name : msg->device_id;
    const char *device_type = msg->device_type[0] != '\0' ? msg->device_type : "generic";
    if (device_store_add(msg->device_id, name, device_type) != 0) {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INTERNAL_ERROR,
                                           "Could not add device %s",
                                           msg->device_id);
        return;
    }

    bool persisted = true;
    if (msg->has_ble_addr &&
        device_store_set_ble_addr(msg->device_id, msg->ble_addr,
                                  msg->ble_addr_type) != 0) {
        device_store_delete(msg->device_id);
        persisted = false;
    }
    if (!persisted) {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INTERNAL_ERROR,
                                           "Could not persist BLE address for %s",
                                           msg->device_id);
        return;
    }

    // BLE connect is a best-effort side effect: persistence alone defines
    // the outcome of add_device (refactor plan §12).
    bool connect_requested = false;
    device_entry_t entry;
    if (device_store_get(msg->device_id, &entry) == 0 && entry.has_ble_addr) {
        connect_requested =
            ble_central_connect(entry.device_id, entry.ble_addr,
                                entry.ble_addr_type) == 0;
    }

    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"%s\",\"persisted\":%s,\"connect_requested\":%s}",
             msg->device_id, persisted ? "true" : "false",
             connect_requested ? "true" : "false");
    command_dispatcher_set_json_result(result, DISPATCH_STATUS_OK, payload);
}

static void cmd_delete_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id || msg->device_id[0] == '\0') {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INVALID_ARGUMENT,
                                           "Missing device_id");
        return;
    }

    // Snapshot peer identity BEFORE removing the store entry so the BLE
    // layer never has to look up a deleted device (refactor plan §8.1).
    device_entry_t existing;
    if (device_store_get(msg->device_id, &existing) != 0) {
        command_dispatcher_set_text_result(result, DISPATCH_STATUS_NOT_FOUND,
                                           "Device %s not found", msg->device_id);
        return;
    }

    int forget_rc = ble_central_forget_peer(
        existing.device_id, existing.ble_addr, existing.ble_addr_type,
        existing.has_ble_addr != 0);
    if (forget_rc != 0) {
        // Store entry is kept intact so the operation can be retried.
        ESP_LOGE(TAG, "[DEVICE_DELETE_FAILED] device=%s could not forget BLE peer rc=%d",
                 msg->device_id, forget_rc);
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_TRANSPORT_ERROR,
                                           "Could not forget BLE peer for %s",
                                           msg->device_id);
        return;
    }

    int rc = device_store_delete(msg->device_id);
    if (rc != 0) {
        // Rare: bond already removed but config remains. Surface loudly.
        ESP_LOGE(TAG, "[DEVICE_DELETE_FAILED] device=%s bond removed but store delete failed",
                 msg->device_id);
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INTERNAL_ERROR,
                                           "Bond removed but device %s remains configured",
                                           msg->device_id);
        return;
    }

    command_dispatcher_set_text_result(result, DISPATCH_STATUS_OK,
                                       "Device %s deleted", msg->device_id);
}

static void cmd_edit_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id || msg->device_id[0] == '\0') {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INVALID_ARGUMENT,
                                           "Missing device_id");
        return;
    }

    const char *name = msg->name[0] != '\0' ? msg->name : NULL;
    const char *device_type = msg->device_type[0] != '\0' ? msg->device_type : NULL;
    if (name == NULL && device_type == NULL) {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INVALID_ARGUMENT,
                                           "Provide name or device_type to edit");
        return;
    }

    int rc = device_store_edit(msg->device_id, name, device_type);
    if (rc != 0) {
        command_dispatcher_set_text_result(result, DISPATCH_STATUS_NOT_FOUND,
                                           "Device %s not found", msg->device_id);
        return;
    }
    command_dispatcher_set_text_result(result, DISPATCH_STATUS_OK,
                                       "Device %s updated", msg->device_id);
}

static void cmd_list_devices(const gw_message_t *msg, dispatch_result_t *result)
{
    (void)msg;
    device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
    int count = device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES);
    if (count < 0) {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INTERNAL_ERROR,
                                           "Could not read device list");
        return;
    }

    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INTERNAL_ERROR,
                                           "Out of memory");
        return;
    }

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) continue;
        cJSON_AddStringToObject(item, "device_id", devices[i].device_id);
        cJSON_AddStringToObject(item, "name", devices[i].name);
        cJSON_AddStringToObject(item, "type", devices[i].type);
        cJSON_AddBoolToObject(item, "connected", devices[i].connected != 0);
        cJSON_AddBoolToObject(item, "has_ble_addr", devices[i].has_ble_addr != 0);
        if (devices[i].has_ble_addr) {
            char address[18];
            format_ble_addr(devices[i].ble_addr, address);
            cJSON_AddStringToObject(item, "ble_addr", address);
            cJSON_AddNumberToObject(item, "ble_addr_type", devices[i].ble_addr_type);
        }
        cJSON_AddItemToArray(array, item);
    }

    bool printed = cJSON_PrintPreallocated(array, result->payload,
                                           sizeof(result->payload), false);
    cJSON_Delete(array);
    if (!printed) {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INTERNAL_ERROR,
                                           "Device list is too large");
        return;
    }
    result->status = DISPATCH_STATUS_OK;
    result->format = DISPATCH_RESULT_JSON;
}

static void cmd_get_status(const gw_message_t *msg, dispatch_result_t *result)
{
    (void)msg;
    gateway_status_t status;
    if (gateway_status_get(&status) != ESP_OK) {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INTERNAL_ERROR,
                                           "Could not read gateway status");
        return;
    }

    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"status\":\"ok\",\"device_count\":%d,"
             "\"connected_count\":%d,\"ble_link_count\":%d}",
             status.device_count, status.connected_count,
             status.ble_link_count);
    command_dispatcher_set_json_result(result, DISPATCH_STATUS_OK, payload);
}

int gateway_commands_register_defaults(void)
{
    if (command_registry_register("add_device", cmd_add_device) != 0 ||
        command_registry_register("delete_device", cmd_delete_device) != 0 ||
        command_registry_register("edit_device", cmd_edit_device) != 0 ||
        command_registry_register("list_devices", cmd_list_devices) != 0 ||
        command_registry_register("get_status", cmd_get_status) != 0) {
        return -1;
    }
    return 0;
}

void gateway_command_handle(const gw_message_t *msg, dispatch_result_t *result)
{
    gateway_command_fn_t fn = command_registry_find(msg->command);
    if (fn == NULL) {
        command_dispatcher_set_text_result(result, DISPATCH_STATUS_NOT_FOUND,
                                           "Unknown gateway command: %s",
                                           msg->command);
        return;
    }
    fn(msg, result);
}
