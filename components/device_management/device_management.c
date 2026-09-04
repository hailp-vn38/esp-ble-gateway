#include "device_management_internal.h"

#include <string.h>

#include "device_command_service.h"
#include "device_state.h"

static device_management_hooks_t s_hooks;
static bool s_hooks_ready;

static void reset_hooks(void)
{
    s_hooks = (device_management_hooks_t) {
        .connect = ble_central_connect,
        .get_status = ble_central_get_device_status,
        .forget_peer = ble_central_forget_peer,
        .schema_get = device_schema_get,
        .schema_forget = device_schema_forget,
        .state_forget = device_state_forget,
        .cancel_commands = device_command_service_cancel_device,
        .store_delete = device_store_delete,
        .publish = gateway_events_publish,
    };
    s_hooks_ready = true;
}

static void ensure_hooks(void)
{
    if (!s_hooks_ready) reset_hooks();
}

void device_management_set_hooks(const device_management_hooks_t *hooks)
{
    reset_hooks();
    if (hooks == NULL) return;
#define OVERRIDE(name) do { if (hooks->name != NULL) s_hooks.name = hooks->name; } while (0)
    OVERRIDE(connect);
    OVERRIDE(get_status);
    OVERRIDE(forget_peer);
    OVERRIDE(schema_get);
    OVERRIDE(schema_forget);
    OVERRIDE(state_forget);
    OVERRIDE(cancel_commands);
    OVERRIDE(store_delete);
    OVERRIDE(publish);
#undef OVERRIDE
}

static device_mgmt_status_t map_store_result(device_store_result_t result)
{
    switch (result) {
    case DEVICE_STORE_OK: return DEVICE_MGMT_OK;
    case DEVICE_STORE_ERR_INVALID_ARG: return DEVICE_MGMT_INVALID_ARG;
    case DEVICE_STORE_ERR_NOT_FOUND: return DEVICE_MGMT_NOT_FOUND;
    case DEVICE_STORE_ERR_DUPLICATE_ID:
    case DEVICE_STORE_ERR_DUPLICATE_BLE_IDENTITY: return DEVICE_MGMT_CONFLICT;
    case DEVICE_STORE_ERR_FULL:
    case DEVICE_STORE_ERR_CAPACITY_EXCEEDED: return DEVICE_MGMT_CAPACITY;
    case DEVICE_STORE_ERR_BUSY: return DEVICE_MGMT_BUSY;
    default: return DEVICE_MGMT_INTERNAL;
    }
}

static void publish_lifecycle(gateway_event_type_t type, const char *device_id)
{
    gateway_event_t event = { .type = type };
    strlcpy(event.device_id, device_id, sizeof(event.device_id));
    s_hooks.publish(&event);
}

device_mgmt_add_result_t device_management_add(
    const device_mgmt_add_request_t *request)
{
    ensure_hooks();
    device_mgmt_add_result_t result = { .status = DEVICE_MGMT_INVALID_ARG };
    if (request == NULL || request->device_id[0] == '\0') return result;

    const char *name = request->name[0] != '\0' ? request->name
                                                 : request->device_id;
    device_store_result_t store_result =
        device_store_add(request->device_id, name);
    if (store_result != DEVICE_STORE_OK) {
        result.status = map_store_result(store_result);
        return result;
    }
    if (request->has_ble_identity) {
        store_result = device_store_set_ble_identity(
            request->device_id, request->ble_addr, request->ble_addr_type);
        if (store_result != DEVICE_STORE_OK) {
            device_store_result_t rollback =
                s_hooks.store_delete(request->device_id);
            result.persisted = rollback != DEVICE_STORE_OK;
            result.status = result.persisted ? DEVICE_MGMT_DEGRADED
                                             : map_store_result(store_result);
            return result;
        }
    }

    result.status = DEVICE_MGMT_OK;
    result.persisted = true;
    if (request->has_ble_identity) {
        result.connect_requested =
            s_hooks.connect(request->device_id, request->ble_addr,
                            request->ble_addr_type) == BLE_CENTRAL_OK;
    }
    publish_lifecycle(GW_EVENT_DEVICE_ADDED, request->device_id);
    return result;
}

device_mgmt_edit_result_t device_management_edit(
    const device_mgmt_edit_request_t *request)
{
    ensure_hooks();
    device_mgmt_edit_result_t result = { .status = DEVICE_MGMT_INVALID_ARG };
    if (request == NULL || request->device_id[0] == '\0' ||
        request->name[0] == '\0') {
        return result;
    }
    result.status = map_store_result(
        device_store_edit(request->device_id, request->name));
    if (result.status == DEVICE_MGMT_OK) {
        result.updated = true;
        publish_lifecycle(GW_EVENT_DEVICE_RENAMED, request->device_id);
    }
    return result;
}

device_mgmt_delete_result_t device_management_delete(const char *device_id)
{
    ensure_hooks();
    device_mgmt_delete_result_t result = { .status = DEVICE_MGMT_INVALID_ARG };
    if (device_id == NULL || device_id[0] == '\0') return result;

    device_entry_t existing = {0};
    device_store_result_t store_result = device_store_get(device_id, &existing);
    if (store_result != DEVICE_STORE_OK) {
        result.status = map_store_result(store_result);
        return result;
    }

    esp_err_t cancel_result = s_hooks.cancel_commands(device_id);
    result.command_cancel_requested =
        cancel_result == ESP_OK || cancel_result == ESP_ERR_INVALID_STATE;

    if (s_hooks.schema_forget(device_id) != ESP_OK) {
        result.status = DEVICE_MGMT_INTERNAL;
        return result;
    }
    result.schema_forgotten = true;

    s_hooks.state_forget(device_id);
    result.state_forgotten = true;
    result.ble_peer_forgotten =
        s_hooks.forget_peer(existing.device_id, existing.ble_addr,
                            existing.ble_addr_type,
                            existing.has_ble_identity) == BLE_CENTRAL_OK;

    store_result = s_hooks.store_delete(device_id);
    result.store_deleted = store_result == DEVICE_STORE_OK;
    if (result.store_deleted) {
        publish_lifecycle(GW_EVENT_DEVICE_REMOVED, device_id);
    } else {
        publish_lifecycle(GW_EVENT_DEVICE_CHANGED, device_id);
    }
    result.status = result.ble_peer_forgotten && result.store_deleted
                        ? DEVICE_MGMT_OK : DEVICE_MGMT_DEGRADED;
    return result;
}

device_mgmt_status_t device_management_snapshot(
    device_inventory_entry_t *out_entries, size_t capacity,
    size_t *out_count)
{
    ensure_hooks();
    if (out_count == NULL || (out_entries == NULL && capacity > 0)) {
        return DEVICE_MGMT_INVALID_ARG;
    }
    device_entry_t stored[DEVICE_STORE_MAX_DEVICES];
    size_t count = 0;
    device_store_result_t store_result = device_store_snapshot(
        out_entries == NULL ? NULL : stored,
        out_entries == NULL ? 0 : DEVICE_STORE_MAX_DEVICES, &count);
    *out_count = count;
    if (store_result != DEVICE_STORE_OK) return map_store_result(store_result);
    if (out_entries == NULL) return DEVICE_MGMT_OK;
    if (capacity < count) return DEVICE_MGMT_CAPACITY;

    for (size_t i = 0; i < count; i++) {
        device_inventory_entry_t *entry = &out_entries[i];
        memset(entry, 0, sizeof(*entry));
        strlcpy(entry->device_id, stored[i].device_id,
                sizeof(entry->device_id));
        strlcpy(entry->name, stored[i].name, sizeof(entry->name));
        entry->has_ble_identity = stored[i].has_ble_identity;
        memcpy(entry->ble_addr, stored[i].ble_addr, sizeof(entry->ble_addr));
        entry->ble_addr_type = stored[i].ble_addr_type;

        ble_central_device_status_t runtime = {0};
        if (s_hooks.get_status(entry->device_id, &runtime) == BLE_CENTRAL_OK) {
            entry->connected = runtime.connected;
            entry->ready = runtime.ready;
        }

        device_schema_snapshot_t schema = {0};
        if (s_hooks.schema_get(entry->device_id, &schema) == ESP_OK) {
            entry->schema_state = schema.state;
            entry->schema_available = schema.has_committed;
            if (schema.has_committed) {
                entry->schema_revision = schema.revision;
                entry->feature_count = (uint8_t)schema.feature_count;
                for (size_t f = 0; f < schema.feature_count; f++) {
                    if (schema.features[f].writable_tool_index >= 0 &&
                        (size_t)schema.features[f].writable_tool_index <
                            schema.tool_count) {
                        entry->writable_feature_count++;
                    }
                }
            }
        }
    }
    return DEVICE_MGMT_OK;
}
