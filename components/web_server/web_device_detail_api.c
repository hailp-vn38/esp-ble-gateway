#include "web_modules.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "ble_central.h"
#include "cJSON.h"
#include "device_schema.h"
#include "device_state.h"
#include "device_store.h"
#include "device_template.h"
#include "gateway_events.h"
#include "mcp_tool_exposure.h"
#include "web_http.h"

static void format_ble_addr(const uint8_t address[6], char output[18])
{
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X", address[5],
             address[4], address[3], address[2], address[1], address[0]);
}

static cJSON *state_to_json(const device_schema_feature_t *feature,
                            const device_state_snapshot_t *snapshot)
{
    cJSON *object = cJSON_CreateObject();
    if (object == NULL) return NULL;

    bool found = false;
    for (size_t i = 0; i < snapshot->count; i++) {
        const device_state_entry_t *entry = &snapshot->entries[i];
        if (strcmp(entry->feature_id, feature->feature_id) == 0 &&
            entry->property_id == feature->property_id) {
            cJSON_AddBoolToObject(object, "valid", entry->valid);
            if (device_template_property_value_type(feature->property_id) ==
                DEVICE_TEMPLATE_VALUE_BOOL) {
                cJSON_AddBoolToObject(object, "value_bool", entry->value_bool);
            } else {
                cJSON_AddNumberToObject(object, "value_int", entry->value_int);
            }
            cJSON_AddNumberToObject(object, "updated_at_ms", entry->updated_at_ms);
            found = true;
            break;
        }
    }
    if (!found) cJSON_AddBoolToObject(object, "valid", false);
    return object;
}

static cJSON *feature_to_json(const char *device_id,
                              const device_schema_feature_t *feature,
                              const device_schema_snapshot_t *schema,
                              const device_state_snapshot_t *state)
{
    cJSON *object = cJSON_CreateObject();
    if (object == NULL) return NULL;

    cJSON_AddStringToObject(object, "feature_id", feature->feature_id);
    cJSON_AddNumberToObject(object, "feature_type", feature->feature_type);
    cJSON_AddNumberToObject(object, "property_id", feature->property_id);

    const device_template_t *template = device_template_resolve(
        feature->feature_type, feature->feature_schema_version);
    cJSON *semantic = cJSON_AddObjectToObject(object, "semantic");
    cJSON *control = cJSON_AddObjectToObject(object, "control");
    cJSON *mcp = cJSON_AddObjectToObject(object, "mcp_control");
    if (semantic == NULL || control == NULL || mcp == NULL) {
        cJSON_Delete(object);
        return NULL;
    }

    const char *property = device_template_property_name(feature->property_id);
    device_template_value_type_t value_type =
        device_template_property_value_type(feature->property_id);
    cJSON_AddStringToObject(semantic, "name",
                            template != NULL ? template->semantic_name : "unknown");
    cJSON_AddStringToObject(semantic, "property", property);
    cJSON_AddStringToObject(semantic, "value_type",
                            value_type == DEVICE_TEMPLATE_VALUE_BOOL ? "bool" :
                            value_type == DEVICE_TEMPLATE_VALUE_INT ? "int" : "none");
    cJSON_AddNumberToObject(semantic, "primary_property",
                            template != NULL ? template->primary_property : 0);

    bool writable = feature->writable_tool_index >= 0 &&
                    (size_t)feature->writable_tool_index < schema->tool_count;
    cJSON_AddBoolToObject(control, "writable", writable);
    if (writable) {
        const device_schema_tool_t *tool =
            &schema->tools[feature->writable_tool_index];
        cJSON_AddStringToObject(control, "write_command", tool->command);
        if (value_type == DEVICE_TEMPLATE_VALUE_INT) {
            cJSON_AddNumberToObject(control, "minimum", tool->min_value);
            cJSON_AddNumberToObject(control, "maximum", tool->max_value);
            cJSON_AddNumberToObject(control, "step", tool->step);
        }
    }

    cJSON *state_object = state_to_json(feature, state);
    if (state_object == NULL) {
        cJSON_Delete(object);
        return NULL;
    }
    cJSON_AddItemToObject(object, "state", state_object);

    mcp_tool_exposure_t exposure;
    bool exposed = mcp_tool_exposure_get_feature(
        device_id, feature->feature_id, &exposure) == ESP_OK;
    cJSON_AddBoolToObject(mcp, "eligible", writable);
    cJSON_AddBoolToObject(mcp, "enabled", exposed && exposure.control_enabled);
    const char *health = !writable ? "missing" :
                         !exposed ? "missing" :
                         exposure.state == MCP_EXPOSURE_ENABLED ? "enabled" :
                         exposure.state == MCP_EXPOSURE_NEEDS_REVIEW ? "needs_review" :
                         "orphaned";
    cJSON_AddStringToObject(mcp, "health", health);
    return object;
}

static esp_err_t detail_get_handler(httpd_req_t *request)
{
    char query[128];
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        web_get_query_value(query, "device_id", device_id, sizeof(device_id)) != ESP_OK ||
        device_id[0] == '\0') {
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }

    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        return web_send_api_error_code(request, "404 Not Found",
                                       "Device not found", "device_not_found");
    }

    const uint32_t event_seq = gateway_events_current_seq();
    device_schema_snapshot_t schema = {0};
    esp_err_t schema_error = device_schema_get(device_id, &schema);
    if (schema_error != ESP_OK && schema_error != ESP_ERR_NOT_FOUND) {
        return web_send_api_error_code(request, "500 Internal Server Error",
                                       "Failed to read schema", "internal_error");
    }

    ble_central_device_status_t ble = {0};
    (void)ble_central_get_device_status(device_id, &ble);
    device_state_snapshot_t state = {0};
    (void)device_state_snapshot(device_id, &state);

    cJSON *root = cJSON_CreateObject();
    cJSON *connection = cJSON_CreateObject();
    cJSON *schema_json = cJSON_CreateObject();
    cJSON *features = cJSON_CreateArray();
    cJSON *mcp = cJSON_CreateObject();
    if (root == NULL || connection == NULL || schema_json == NULL ||
        features == NULL || mcp == NULL) {
        cJSON_Delete(root); cJSON_Delete(connection); cJSON_Delete(schema_json);
        cJSON_Delete(features); cJSON_Delete(mcp);
        return web_send_api_error(request, "500 Internal Server Error", "Out of memory");
    }

    cJSON_AddStringToObject(root, "device_id", entry.device_id);
    cJSON_AddStringToObject(root, "name", entry.name);
    if (entry.has_ble_identity) {
        char address[18];
        format_ble_addr(entry.ble_addr, address);
        cJSON_AddStringToObject(root, "ble_addr", address);
        cJSON_AddNumberToObject(root, "ble_addr_type", entry.ble_addr_type);
    }
    cJSON_AddBoolToObject(connection, "connected", ble.connected);
    cJSON_AddBoolToObject(connection, "ready", ble.ready);
    cJSON_AddItemToObject(root, "connection", connection);
    cJSON_AddStringToObject(schema_json, "state", device_schema_state_name(schema.state));
    cJSON_AddNumberToObject(schema_json, "revision", schema.revision);
    cJSON_AddBoolToObject(schema_json, "has_committed", schema.has_committed);
    cJSON_AddNumberToObject(schema_json, "updated_at_ms", schema.updated_at_ms);
    cJSON_AddItemToObject(root, "schema", schema_json);

    if (schema.has_committed) {
        for (size_t i = 0; i < schema.feature_count; i++) {
            cJSON *feature = feature_to_json(device_id, &schema.features[i],
                                             &schema, &state);
            if (feature != NULL) cJSON_AddItemToArray(features, feature);
        }
    }
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(mcp, "tool", "device_control");
    cJSON_AddNumberToObject(mcp, "policy_revision",
                            mcp_tool_exposure_get_policy_revision());
    cJSON_AddItemToObject(root, "mcp", mcp);

    char seq_text[16];
    snprintf(seq_text, sizeof(seq_text), "%" PRIu32, event_seq);
    httpd_resp_set_hdr(request, "X-Gateway-Event-Seq", seq_text);
    return web_send_json(request, root);
}

esp_err_t web_device_detail_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        WEB_URI_INIT("/api/devices/detail", HTTP_GET, detail_get_handler),
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
