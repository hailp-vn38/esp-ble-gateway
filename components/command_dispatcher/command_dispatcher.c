#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ble_central.h"
#include "command_dispatcher.h"
#include "device_store.h"
#include "log_buffer.h"

static const char *TAG = "dispatcher";

#define DISPATCHER_MAX_PENDING_ACKS DEVICE_STORE_MAX_DEVICES

typedef struct {
    char command_name[GW_MSG_COMMAND_LEN];
    gateway_command_fn_t fn;
} registry_entry_t;

typedef struct {
    bool in_use;
    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];
    gw_message_t response;
    SemaphoreHandle_t semaphore;
} pending_ack_t;

static registry_entry_t s_registry[DISPATCHER_MAX_COMMANDS];
static int s_registry_count;
static SemaphoreHandle_t s_registry_mutex;
static SemaphoreHandle_t s_ack_mutex;
static pending_ack_t s_pending_acks[DISPATCHER_MAX_PENDING_ACKS];

static void set_result(dispatch_result_t *result, bool success, const char *format, ...)
{
    result->success = success;
    va_list args;
    va_start(args, format);
    vsnprintf(result->message, sizeof(result->message), format, args);
    va_end(args);
}

static void format_ble_addr(const uint8_t address[6], char output[18])
{
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[5], address[4], address[3], address[2], address[1], address[0]);
}

static void cmd_add_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        set_result(result, false, "Missing device_id");
        return;
    }

    const char *name = msg->name[0] != '\0' ? msg->name : msg->device_id;
    const char *device_type = msg->device_type[0] != '\0' ? msg->device_type : "generic";
    if (device_store_add(msg->device_id, name, device_type) != 0) {
        set_result(result, false, "Could not add device %s", msg->device_id);
        return;
    }

    if (msg->has_ble_addr &&
        device_store_set_ble_addr(msg->device_id, msg->ble_addr,
                                  msg->ble_addr_type) != 0) {
        device_store_delete(msg->device_id);
        set_result(result, false, "Could not persist BLE address for %s", msg->device_id);
        return;
    }

    device_entry_t entry;
    if (device_store_get(msg->device_id, &entry) == 0 && entry.has_ble_addr) {
        ble_central_connect(entry.device_id, entry.ble_addr, entry.ble_addr_type);
    }
    set_result(result, true, "Device %s added", msg->device_id);
}

static void cmd_delete_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        set_result(result, false, "Missing device_id");
        return;
    }
    device_entry_t existing;
    if (device_store_get(msg->device_id, &existing) != 0) {
        set_result(result, false, "Device %s not found", msg->device_id);
        return;
    }

    int rc = device_store_delete(msg->device_id);
    if (rc == 0) ble_central_forget_device(msg->device_id);
    set_result(result, rc == 0, rc == 0 ? "Device %s deleted" : "Could not delete %s",
               msg->device_id);
}

static void cmd_edit_device(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        set_result(result, false, "Missing device_id");
        return;
    }
    const char *name = msg->name[0] != '\0' ? msg->name : NULL;
    const char *device_type = msg->device_type[0] != '\0' ? msg->device_type : NULL;
    if (name == NULL && device_type == NULL) {
        set_result(result, false, "Provide name or device_type to edit");
        return;
    }
    int rc = device_store_edit(msg->device_id, name, device_type);
    set_result(result, rc == 0, rc == 0 ? "Device %s updated" : "Device %s not found",
               msg->device_id);
}

static void cmd_list_devices(const gw_message_t *msg, dispatch_result_t *result)
{
    device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
    int count = device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES);
    if (count < 0) {
        set_result(result, false, "Could not read device list");
        return;
    }

    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        set_result(result, false, "Out of memory");
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
    if (!printed) strlcpy(result->message, "Device list is too large", sizeof(result->message));
}

static void cmd_get_status(const gw_message_t *msg, dispatch_result_t *result)
{
    device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
    int count = device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES);
    if (count < 0) {
        set_result(result, false, "Could not read gateway status");
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
        if (!s_pending_acks[i].in_use && available == NULL) available = &s_pending_acks[i];
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
        xSemaphoreTake(s_ack_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    pending->in_use = false;
    pending->device_id[0] = '\0';
    pending->command[0] = '\0';
    xSemaphoreGive(s_ack_mutex);
}

static void handle_device_command(const gw_message_t *msg, dispatch_result_t *result)
{
    if (!msg->has_device_id) {
        set_result(result, false, "Missing device_id");
        return;
    }
    if (!ble_central_is_connected(msg->device_id)) {
        set_result(result, false, "Device %s is not connected", msg->device_id);
        return;
    }

    pending_ack_t *pending = allocate_pending_ack(msg);
    if (pending == NULL) {
        set_result(result, false, "Another command for %s is pending", msg->device_id);
        return;
    }

    int send_rc = ble_central_send_command(msg->device_id, msg);
    char log_line[LOG_ENTRY_MAX_LEN];
    snprintf(log_line, sizeof(log_line), "[SENT] device=%s command=%s success=%d",
             msg->device_id, msg->command, send_rc == 0);
    log_buffer_push(log_line);

    if (send_rc != 0) {
        release_pending_ack(pending);
        set_result(result, false, "Could not send command to %s", msg->device_id);
        return;
    }

    if (xSemaphoreTake(pending->semaphore,
                       pdMS_TO_TICKS(DISPATCHER_ACK_TIMEOUT_MS)) != pdTRUE) {
        release_pending_ack(pending);
        set_result(result, false, "No ACK from %s within %d ms", msg->device_id,
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
        set_result(result, false, "Could not read ACK from %s", msg->device_id);
        return;
    }

    set_result(result, response.bool_value != 0,
               response.bool_value ? "Device %s acknowledged '%s'"
                                   : "Device %s rejected '%s'",
               msg->device_id, msg->command);
}

static void handle_gateway_command(const gw_message_t *msg, dispatch_result_t *result)
{
    gateway_command_fn_t function = NULL;
    if (xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        for (int i = 0; i < s_registry_count; i++) {
            if (strcmp(s_registry[i].command_name, msg->command) == 0) {
                function = s_registry[i].fn;
                break;
            }
        }
        xSemaphoreGive(s_registry_mutex);
    }
    if (function == NULL) {
        set_result(result, false, "Unknown gateway command: %s", msg->command);
        return;
    }
    function(msg, result);
}

int command_dispatcher_register(const char *command_name, gateway_command_fn_t fn)
{
    if (command_name == NULL || fn == NULL || command_name[0] == '\0' ||
        strnlen(command_name, GW_MSG_COMMAND_LEN) >= GW_MSG_COMMAND_LEN ||
        s_registry_mutex == NULL ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }

    for (int i = 0; i < s_registry_count; i++) {
        if (strcmp(s_registry[i].command_name, command_name) == 0) {
            xSemaphoreGive(s_registry_mutex);
            return -1;
        }
    }
    if (s_registry_count >= DISPATCHER_MAX_COMMANDS) {
        xSemaphoreGive(s_registry_mutex);
        return -1;
    }
    strlcpy(s_registry[s_registry_count].command_name, command_name,
            sizeof(s_registry[s_registry_count].command_name));
    s_registry[s_registry_count++].fn = fn;
    xSemaphoreGive(s_registry_mutex);
    return 0;
}

int command_dispatcher_init(void)
{
    if (s_registry_mutex == NULL) s_registry_mutex = xSemaphoreCreateMutex();
    if (s_ack_mutex == NULL) s_ack_mutex = xSemaphoreCreateMutex();
    if (s_registry_mutex == NULL || s_ack_mutex == NULL) return -1;

    if (xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return -1;
    memset(s_registry, 0, sizeof(s_registry));
    s_registry_count = 0;
    xSemaphoreGive(s_registry_mutex);

    for (int i = 0; i < DISPATCHER_MAX_PENDING_ACKS; i++) {
        if (s_pending_acks[i].semaphore == NULL) {
            s_pending_acks[i].semaphore = xSemaphoreCreateBinary();
        }
        s_pending_acks[i].in_use = false;
        if (s_pending_acks[i].semaphore == NULL) return -1;
        while (xSemaphoreTake(s_pending_acks[i].semaphore, 0) == pdTRUE) {}
    }

    if (command_dispatcher_register("add_device", cmd_add_device) != 0 ||
        command_dispatcher_register("delete_device", cmd_delete_device) != 0 ||
        command_dispatcher_register("edit_device", cmd_edit_device) != 0 ||
        command_dispatcher_register("list_devices", cmd_list_devices) != 0 ||
        command_dispatcher_register("get_status", cmd_get_status) != 0) {
        return -1;
    }
    ESP_LOGI(TAG, "Command dispatcher initialized with %d commands", s_registry_count);
    return 0;
}

int command_dispatcher_get_registered_names(const char **out_names, int max_names)
{
    if (out_names == NULL || max_names <= 0 ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }
    int count = s_registry_count < max_names ? s_registry_count : max_names;
    for (int i = 0; i < count; i++) out_names[i] = s_registry[i].command_name;
    xSemaphoreGive(s_registry_mutex);
    return count;
}

int command_dispatcher_is_registered(const char *command_name)
{
    if (command_name == NULL ||
        xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return 0;
    int found = 0;
    for (int i = 0; i < s_registry_count; i++) {
        if (strcmp(s_registry[i].command_name, command_name) == 0) {
            found = 1;
            break;
        }
    }
    xSemaphoreGive(s_registry_mutex);
    return found;
}

void command_dispatcher_handle(const gw_message_t *msg, dispatch_result_t *result)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    if (msg == NULL) {
        set_result(result, false, "Null message");
        return;
    }
    if (strcmp(msg->type, "device_command") == 0) {
        handle_device_command(msg, result);
    } else if (strcmp(msg->type, "gateway_command") == 0) {
        handle_gateway_command(msg, result);
    } else {
        set_result(result, false, "Unknown message type: %s", msg->type);
    }
}

void command_dispatcher_on_device_notify(const char *device_id,
                                         const gw_message_t *msg)
{
    if (device_id == NULL || msg == NULL) return;

    bool matched = false;
    if (xSemaphoreTake(s_ack_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
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

    char log_line[LOG_ENTRY_MAX_LEN];
    snprintf(log_line, sizeof(log_line), "%s device=%s command=%s success=%d value=%d",
             matched ? "[ACK]" : "[NOTIFY]", device_id, msg->command,
             msg->bool_value != 0, msg->int_value);
    log_buffer_push(log_line);
    ESP_LOGI(TAG, "%s", log_line);
}
