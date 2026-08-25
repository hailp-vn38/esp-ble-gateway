#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "ble_central.h"
#include "command_dispatcher.h"
#include "command_dispatcher_internal.h"
#include "device_store.h"

static void format_ble_addr(const uint8_t address[6], char output[18])
{
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[5], address[4], address[3], address[2], address[1], address[0]);
}

static void cmd_add_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        command_dispatcher_set_result(result, false, "Missing device_id");
        return;
    }

    const char *name = msg->name[0] != '\0' ? msg->name : msg->device_id;
    const char *device_type = msg->device_type[0] != '\0' ? msg->device_type : "generic";
    if (device_store_add(msg->device_id, name, device_type) != 0) {
        command_dispatcher_set_result(result, false, "Could not add device %s",
                                      msg->device_id);
        return;
    }

    if (msg->has_ble_addr &&
        device_store_set_ble_addr(msg->device_id, msg->ble_addr,
                                  msg->ble_addr_type) != 0) {
        device_store_delete(msg->device_id);
        command_dispatcher_set_result(result, false,
                                      "Could not persist BLE address for %s",
                                      msg->device_id);
        return;
    }

    device_entry_t entry;
    if (device_store_get(msg->device_id, &entry) == 0 && entry.has_ble_addr) {
        ble_central_connect(entry.device_id, entry.ble_addr, entry.ble_addr_type);
    }
    command_dispatcher_set_result(result, true, "Device %s added", msg->device_id);
}

static void cmd_delete_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        command_dispatcher_set_result(result, false, "Missing device_id");
        return;
    }

    device_entry_t existing;
    if (device_store_get(msg->device_id, &existing) != 0) {
        command_dispatcher_set_result(result, false, "Device %s not found",
                                      msg->device_id);
        return;
    }

    int rc = device_store_delete(msg->device_id);
    if (rc == 0) ble_central_forget_device(msg->device_id);
    command_dispatcher_set_result(
        result, rc == 0, rc == 0 ? "Device %s deleted" : "Could not delete %s",
        msg->device_id);
}

static void cmd_edit_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        command_dispatcher_set_result(result, false, "Missing device_id");
        return;
    }

    const char *name = msg->name[0] != '\0' ? msg->name : NULL;
    const char *device_type = msg->device_type[0] != '\0' ? msg->device_type : NULL;
    if (name == NULL && device_type == NULL) {
        command_dispatcher_set_result(result, false,
                                      "Provide name or device_type to edit");
        return;
    }

    int rc = device_store_edit(msg->device_id, name, device_type);
    command_dispatcher_set_result(
        result, rc == 0, rc == 0 ? "Device %s updated" : "Device %s not found",
        msg->device_id);
}

static void cmd_list_devices(const gw_message_t *msg, dispatch_result_t *result)
{
    (void)msg;
    device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
    int count = device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES);
    if (count < 0) {
        command_dispatcher_set_result(result, false, "Could not read device list");
        return;
    }

    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        command_dispatcher_set_result(result, false, "Out of memory");
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

    bool printed = cJSON_PrintPreallocated(array, result->message,
                                           sizeof(result->message), false);
    cJSON_Delete(array);
    result->success = printed;
    if (!printed) {
        strlcpy(result->message, "Device list is too large", sizeof(result->message));
    }
}

static void cmd_get_status(const gw_message_t *msg, dispatch_result_t *result)
{
    (void)msg;
    device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
    int count = device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES);
    if (count < 0) {
        command_dispatcher_set_result(result, false, "Could not read gateway status");
        return;
    }

    int ready_count = 0;
    for (int i = 0; i < count; i++) ready_count += devices[i].connected != 0;
    snprintf(result->message, sizeof(result->message),
             "{\"status\":\"ok\",\"device_count\":%d,"
             "\"connected_count\":%d,\"ble_link_count\":%d}",
             count, ready_count, ble_central_active_count());
    result->success = true;
}

int gateway_commands_register_defaults(void)
{
    if (command_dispatcher_register("add_device", cmd_add_device) != 0 ||
        command_dispatcher_register("delete_device", cmd_delete_device) != 0 ||
        command_dispatcher_register("edit_device", cmd_edit_device) != 0 ||
        command_dispatcher_register("list_devices", cmd_list_devices) != 0 ||
        command_dispatcher_register("get_status", cmd_get_status) != 0) {
        return -1;
    }
    return 0;
}

void gateway_command_handle(const gw_message_t *msg, dispatch_result_t *result)
{
    gateway_command_fn_t fn = command_registry_find(msg->command);
    if (fn == NULL) {
        command_dispatcher_set_result(result, false, "Unknown gateway command: %s",
                                      msg->command);
        return;
    }
    fn(msg, result);
}
