#include "web_modules.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_management.h"
#include "device_store.h"
#include "device_template.h"
#include "gateway_events.h"
#include "mcp_tool_exposure.h"
#include "memory_policy.h"
#include "web_http.h"

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int parse_ble_addr(const char *text, uint8_t address[6])
{
    if (text == NULL) return -1;
    uint8_t display[6];
    for (int i = 0; i < 6; i++) {
        int high = hex_value(*text++);
        int low = high >= 0 ? hex_value(*text++) : -1;
        if (high < 0 || low < 0) return -1;
        display[i] = (uint8_t)((high << 4) | low);
        if (i < 5) {
            if (*text != ':' && *text != '-') return -1;
            text++;
        }
    }
    if (*text != '\0') return -1;
    for (int i = 0; i < 6; i++) address[i] = display[5 - i];
    return 0;
}

static void format_ble_addr(const uint8_t address[6], char output[18])
{
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X", address[5],
             address[4], address[3], address[2], address[1], address[0]);
}

static const char *management_http_status(device_mgmt_status_t status)
{
    switch (status) {
    case DEVICE_MGMT_OK:          return "200 OK";
    case DEVICE_MGMT_INVALID_ARG: return "400 Bad Request";
    case DEVICE_MGMT_NOT_FOUND:   return "404 Not Found";
    case DEVICE_MGMT_CONFLICT:    return "409 Conflict";
    case DEVICE_MGMT_BUSY:        return "409 Conflict";
    case DEVICE_MGMT_CAPACITY:    return "507 Insufficient Storage";
    case DEVICE_MGMT_DEGRADED:    return "207 Multi-Status";
    default:                      return "500 Internal Server Error";
    }
}

static const char *management_error_code(device_mgmt_status_t status)
{
    switch (status) {
    case DEVICE_MGMT_INVALID_ARG: return "invalid_request";
    case DEVICE_MGMT_NOT_FOUND:   return "device_not_found";
    case DEVICE_MGMT_CONFLICT:    return "conflict";
    case DEVICE_MGMT_BUSY:        return "device_busy";
    case DEVICE_MGMT_CAPACITY:    return "store_full";
    case DEVICE_MGMT_DEGRADED:    return "cleanup_degraded";
    default:                      return "internal_error";
    }
}

static cJSON *management_response(device_mgmt_status_t status,
                                  const char *message)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) return NULL;
    bool ok = status == DEVICE_MGMT_OK;
    cJSON_AddBoolToObject(json, "success", ok);
    cJSON_AddNumberToObject(json, "status", (int)status);
    cJSON_AddStringToObject(json, "message", message != NULL ? message : "");
    if (!ok) {
        cJSON *error = cJSON_AddObjectToObject(json, "error");
        if (error != NULL) {
            cJSON_AddStringToObject(error, "code",
                                    management_error_code(status));
        }
    }
    return json;
}

static esp_err_t send_management_response(httpd_req_t *request,
                                          device_mgmt_status_t status,
                                          const char *message, cJSON *data)
{
    if (status != DEVICE_MGMT_OK) {
        httpd_resp_set_status(request, management_http_status(status));
    }
    cJSON *json = management_response(status, message);
    if (json != NULL && data != NULL) cJSON_AddItemToObject(json, "data", data);
    else cJSON_Delete(data);
    return web_send_json(request, json);
}

static cJSON *serialize_inventory_entry(const device_inventory_entry_t *entry)
{
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) return NULL;
    cJSON_AddStringToObject(item, "device_id", entry->device_id);
    cJSON_AddStringToObject(item, "name", entry->name);
    cJSON_AddBoolToObject(item, "connected", entry->connected);
    cJSON_AddBoolToObject(item, "ready", entry->ready);

    cJSON *capabilities = cJSON_AddObjectToObject(item, "capabilities");
    if (capabilities != NULL) {
        cJSON_AddBoolToObject(capabilities, "available",
                              entry->schema_available);
        cJSON_AddStringToObject(capabilities, "state",
                                device_schema_state_name(entry->schema_state));
        cJSON_AddNumberToObject(capabilities, "feature_count",
                                entry->schema_available ? entry->feature_count : 0);
        cJSON_AddNumberToObject(
            capabilities, "writable_feature_count",
            entry->schema_available ? entry->writable_feature_count : 0);
        if (entry->schema_available) {
            cJSON_AddNumberToObject(capabilities, "revision",
                                    entry->schema_revision);
        }
    }

    cJSON *controls = cJSON_AddArrayToObject(item, "controls");
    mcp_control_hint_t hints[MCP_SEMANTIC_CONTROL_HINT_MAX] = {0};
    size_t hint_count = 0;
    bool controls_truncated = false;
    if (controls != NULL &&
        mcp_semantic_control_get_hints(
            entry->device_id, hints, MCP_SEMANTIC_CONTROL_HINT_MAX,
            &hint_count, &controls_truncated) == ESP_OK) {
        if (mcp_semantic_control_serialize_hints(controls, hints,
                                                hint_count) != ESP_OK)
            controls_truncated = true;
    }
    if (controls_truncated)
        cJSON_AddBoolToObject(item, "controls_truncated", true);

    cJSON_AddBoolToObject(item, "has_ble_addr", entry->has_ble_identity);
    if (entry->has_ble_identity) {
        char address[18];
        format_ble_addr(entry->ble_addr, address);
        cJSON_AddStringToObject(item, "ble_addr", address);
        cJSON_AddNumberToObject(item, "ble_addr_type", entry->ble_addr_type);
    }
    return item;
}

static esp_err_t devices_get_handler(httpd_req_t *request)
{
    uint32_t base_seq = gateway_events_current_seq();
    device_inventory_entry_t *entries = gw_mem_calloc(
        DEVICE_STORE_MAX_DEVICES, sizeof(*entries), GW_MEM_EXTERNAL_PREFERRED);
    if (entries == NULL) {
        return web_send_api_error(request, "503 Service Unavailable",
                                  "Could not allocate response workspace");
    }
    size_t count = 0;
    device_mgmt_status_t status = device_management_snapshot(
        entries, DEVICE_STORE_MAX_DEVICES, &count);
    if (status != DEVICE_MGMT_OK) {
        gw_mem_free(entries);
        return send_management_response(request, status,
                                        "Could not read device inventory", NULL);
    }

    cJSON *array = cJSON_CreateArray();
    for (size_t i = 0; array != NULL && i < count; i++) {
        cJSON *item = serialize_inventory_entry(&entries[i]);
        if (item == NULL) {
            cJSON_Delete(array);
            array = NULL;
            break;
        }
        cJSON_AddItemToArray(array, item);
    }
    gw_mem_free(entries);
    if (array == NULL) {
        return web_send_api_error(request, "500 Internal Server Error",
                                  "Could not serialize device inventory");
    }

    char seq_text[16];
    snprintf(seq_text, sizeof(seq_text), "%" PRIu32, base_seq);
    httpd_resp_set_hdr(request, "X-Gateway-Event-Seq", seq_text);
    return web_send_json(request, array);
}

static esp_err_t devices_write_handler(httpd_req_t *request)
{
    char body[WEB_DEVICE_BODY_MAX_LEN];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) return web_send_body_error(request, body_status);

    if (request->method == HTTP_POST) {
        device_mgmt_add_request_t typed = {0};
        const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(json, "name");
        const char *device_id = web_get_json_string(
            json, "device_id", sizeof(typed.device_id), true);
        const char *name = name_item != NULL
                               ? web_get_json_string(
                                     json, "name", sizeof(typed.name), true)
                               : NULL;
        if (device_id == NULL || (name_item != NULL && name == NULL)) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                            "Invalid device fields",
                                            "invalid_request");
        }
        strlcpy(typed.device_id, device_id, sizeof(typed.device_id));
        if (name != NULL) strlcpy(typed.name, name, sizeof(typed.name));

        if (cJSON_GetObjectItemCaseSensitive(json, "ble_addr") != NULL) {
            const char *address = web_get_json_string(json, "ble_addr", 18, true);
            const cJSON *address_type =
                cJSON_GetObjectItemCaseSensitive(json, "ble_addr_type");
            if (address == NULL || parse_ble_addr(address, typed.ble_addr) != 0 ||
                (address_type != NULL &&
                 (!cJSON_IsNumber(address_type) || address_type->valueint < 0 ||
                  address_type->valueint > DEVICE_STORE_BLE_ADDR_TYPE_MAX ||
                  address_type->valuedouble != (double)address_type->valueint))) {
                cJSON_Delete(json);
                return web_send_api_error_code(request, "400 Bad Request",
                                                "Invalid device fields",
                                                "invalid_request");
            }
            typed.has_ble_identity = true;
            typed.ble_addr_type =
                address_type != NULL ? (uint8_t)address_type->valueint : 0;
        }
        cJSON_Delete(json);
        device_mgmt_add_result_t result = device_management_add(&typed);
        cJSON *data = cJSON_CreateObject();
        if (data != NULL) {
            cJSON_AddStringToObject(data, "device_id", typed.device_id);
            cJSON_AddBoolToObject(data, "persisted", result.persisted);
            cJSON_AddBoolToObject(data, "connect_requested",
                                  result.connect_requested);
        }
        return send_management_response(request, result.status, "", data);
    }

    device_mgmt_edit_request_t typed = {0};
    const char *device_id = web_get_json_string(
        json, "device_id", sizeof(typed.device_id), true);
    const char *name = web_get_json_string(json, "name", sizeof(typed.name), true);
    if (device_id == NULL || name == NULL) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                        "Invalid device fields",
                                        "invalid_request");
    }
    strlcpy(typed.device_id, device_id, sizeof(typed.device_id));
    strlcpy(typed.name, name, sizeof(typed.name));
    cJSON_Delete(json);
    device_mgmt_edit_result_t result = device_management_edit(&typed);
    return send_management_response(request, result.status,
                                    result.updated ? "Device updated" : "", NULL);
}

static esp_err_t devices_delete_handler(httpd_req_t *request)
{
    char query[128];
    char device_id[DEVICE_ID_MAX_LEN] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        web_get_query_value(query, "device_id", device_id,
                            sizeof(device_id)) != ESP_OK ||
        device_id[0] == '\0') {
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }

    device_mgmt_delete_result_t result = device_management_delete(device_id);
    cJSON *data = cJSON_CreateObject();
    if (data != NULL) {
        cJSON_AddBoolToObject(data, "command_cancel_requested",
                              result.command_cancel_requested);
        cJSON_AddBoolToObject(data, "schema_forgotten", result.schema_forgotten);
        cJSON_AddBoolToObject(data, "state_forgotten", result.state_forgotten);
        cJSON_AddBoolToObject(data, "ble_peer_forgotten",
                              result.ble_peer_forgotten);
        cJSON_AddBoolToObject(data, "store_deleted", result.store_deleted);
    }
    return send_management_response(
        request, result.status,
        result.status == DEVICE_MGMT_OK ? "Device deleted" : "", data);
}

esp_err_t web_device_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        WEB_URI_INIT("/api/devices", HTTP_GET, devices_get_handler),
        WEB_URI_INIT("/api/devices", HTTP_POST, devices_write_handler),
        WEB_URI_INIT("/api/devices", HTTP_PUT, devices_write_handler),
        WEB_URI_INIT("/api/devices", HTTP_DELETE, devices_delete_handler),
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
