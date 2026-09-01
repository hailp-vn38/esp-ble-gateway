#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "ble_central.h"
#include "gateway_status.h"

#include "command_dispatcher.h"
#include "command_dispatcher_internal.h"
#include "device_store.h"
#include "device_schema.h"
#include "device_state.h"
#include "mcp_tool_exposure.h"

static const char *TAG = "dispatcher";

static void format_ble_addr(const uint8_t address[6], char output[18])
{
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[5], address[4], address[3], address[2], address[1], address[0]);
}

// Typed Device Store contract -> dispatcher status (refactor plan §11).
static dispatch_status_t status_for_store_result(device_store_result_t result)
{
    switch (result) {
    case DEVICE_STORE_OK:
        return DISPATCH_STATUS_OK;
    case DEVICE_STORE_ERR_INVALID_ARG:
        return DISPATCH_STATUS_INVALID_ARGUMENT;
    case DEVICE_STORE_ERR_NOT_FOUND:
        return DISPATCH_STATUS_NOT_FOUND;
    case DEVICE_STORE_ERR_DUPLICATE_ID:
    case DEVICE_STORE_ERR_DUPLICATE_BLE_IDENTITY:
        return DISPATCH_STATUS_CONFLICT;
    case DEVICE_STORE_ERR_FULL:
    case DEVICE_STORE_ERR_CAPACITY_EXCEEDED:
        return DISPATCH_STATUS_RESOURCE_EXHAUSTED;
    case DEVICE_STORE_ERR_BUSY:
        return DISPATCH_STATUS_BUSY;
    default:
        // PERSISTENCE/CORRUPT/SCHEMA_TOO_NEW/BUFFER_TOO_SMALL/INVALID_STATE
        return DISPATCH_STATUS_INTERNAL_ERROR;
    }
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
    // device_store_add takes (device_id, name) — no device-level type.
    device_store_result_t store_rc =
        device_store_add(msg->device_id, name);
    if (store_rc != DEVICE_STORE_OK) {
        command_dispatcher_set_text_result(result, status_for_store_result(store_rc),
                                           "Could not add device %s",
                                           msg->device_id);
        return;
    }

    if (msg->has_ble_addr) {
        store_rc = device_store_set_ble_identity(msg->device_id, msg->ble_addr,
                                                 msg->ble_addr_type);
        if (store_rc != DEVICE_STORE_OK) {
            device_store_delete(msg->device_id);
            if (store_rc == DEVICE_STORE_ERR_DUPLICATE_BLE_IDENTITY) {
                command_dispatcher_set_text_result(
                    result, DISPATCH_STATUS_CONFLICT,
                    "BLE address already registered to another device");
            } else {
                command_dispatcher_set_text_result(
                    result, status_for_store_result(store_rc),
                    "Could not persist BLE identity for %s", msg->device_id);
            }
            return;
        }
    }

    // BLE connect is a best-effort side effect: persistence alone defines
    // the outcome of add_device (refactor plan §12).
    bool connect_requested = false;
    device_entry_t entry;
    if (device_store_get(msg->device_id, &entry) == DEVICE_STORE_OK &&
        entry.has_ble_identity) {
        connect_requested =
            ble_central_connect(entry.device_id, entry.ble_addr,
                                entry.ble_addr_type) == 0;
    }

    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"%s\",\"persisted\":true,\"connect_requested\":%s}",
             msg->device_id, connect_requested ? "true" : "false");
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

    device_entry_t existing;
    device_store_result_t store_rc = device_store_get(msg->device_id, &existing);
    if (store_rc != DEVICE_STORE_OK) {
        command_dispatcher_set_text_result(result,
                                           status_for_store_result(store_rc),
                                           "Device %s not found", msg->device_id);
        return;
    }

    /* Step 1: MCP exposure revoke MUST happen first (spec §14/§29). */
    esp_err_t exposure_rc = mcp_tool_exposure_forget_device(msg->device_id);
    if (exposure_rc != ESP_OK) {
        ESP_LOGW(TAG, "[DEVICE_DELETE] device=%s exposure revoke failed: %s (continuing)",
                 msg->device_id, esp_err_to_name(exposure_rc));
    }

    /* Step 2: schema forget MUST succeed (failure-safe per spec §14/§16). */
    esp_err_t schema_rc = device_schema_forget(msg->device_id);
    if (schema_rc != ESP_OK) {
        ESP_LOGE(TAG, "[DEVICE_DELETE_FAILED] device=%s schema forget failed: %s",
                 msg->device_id, esp_err_to_name(schema_rc));
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INTERNAL_ERROR,
                                           "Could not forget schema for %s",
                                           msg->device_id);
        return;
    }

    /* Step 2b: Clear runtime feature state. Failure is acceptable degradation. */
    device_state_forget(msg->device_id);

    /* Step 3: BLE peer forget. Failure is acceptable degradation. */
    int forget_rc = ble_central_forget_peer(
        existing.device_id, existing.ble_addr, existing.ble_addr_type,
        existing.has_ble_identity);
    if (forget_rc != 0) {
        ESP_LOGW(TAG, "[DEVICE_DELETE_DEGRADED] device=%s BLE peer forget failed rc=%d",
                 msg->device_id, forget_rc);
    }

    /* Step 4: device store delete. Failure is acceptable degradation. */
    device_store_result_t delete_rc = device_store_delete(msg->device_id);
    if (delete_rc != DEVICE_STORE_OK) {
        ESP_LOGW(TAG, "[DEVICE_DELETE_DEGRADED] device=%s store delete failed",
                 msg->device_id);
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
    if (name == NULL) {
        command_dispatcher_set_text_result(result,
                                           DISPATCH_STATUS_INVALID_ARGUMENT,
                                           "Provide name to edit");
        return;
    }

    device_store_result_t edit_rc =
        device_store_edit(msg->device_id, name);
    if (edit_rc != DEVICE_STORE_OK) {
        command_dispatcher_set_text_result(result,
                                           status_for_store_result(edit_rc),
                                           "Device %s not found", msg->device_id);
        return;
    }
    if (name != NULL) {
        esp_err_t refresh_rc =
            mcp_tool_exposure_refresh_device_name(msg->device_id);
        if (refresh_rc != ESP_OK && refresh_rc != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "[DEVICE_EDIT] MCP name refresh failed for %s: %s",
                     msg->device_id, esp_err_to_name(refresh_rc));
        }
    }
    command_dispatcher_set_text_result(result, DISPATCH_STATUS_OK,
                                       "Device %s updated", msg->device_id);
}

static void cmd_list_devices(const gw_message_t *msg, dispatch_result_t *result)
{
    (void)msg;
    device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
    size_t count = 0;
    if (device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES, &count) !=
        DEVICE_STORE_OK) {
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

    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) continue;

        // Connection state is merged from the BLE runtime snapshot, never
        // read from the persistent store (refactor plan §10.4).
        ble_central_device_status_t status;
        bool connected =
            ble_central_get_device_status(devices[i].device_id, &status) ==
                BLE_CENTRAL_OK &&
            status.connected;

        cJSON_AddStringToObject(item, "device_id", devices[i].device_id);
        cJSON_AddStringToObject(item, "name", devices[i].name);
        cJSON_AddBoolToObject(item, "connected", connected);
        cJSON_AddBoolToObject(item, "has_ble_addr", devices[i].has_ble_identity);
        if (devices[i].has_ble_identity) {
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

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"status\":\"ok\",\"device_count\":%d,"
             "\"connected_count\":%d,\"ble_link_count\":%d,"
             "\"internal\":{\"free\":%" PRIu32 ",\"min_free\":%" PRIu32 ","
             "\"largest_free_block\":%" PRIu32 "},"
             "\"psram\":{\"ready\":%s,\"free\":%" PRIu32 ",\"min_free\":%" PRIu32 ","
             "\"largest_free_block\":%" PRIu32 "}}",
             status.device_count, status.connected_count,
             status.ble_link_count,
             status.internal_free, status.internal_min_free,
             status.internal_largest_free_block,
             status.psram_ready ? "true" : "false",
             status.psram_free, status.psram_min_free,
             status.psram_largest_free_block);
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
