#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "ble_central.h"
#include "cbor_codec.h"
#include "device_schema.h"
#include "device_state.h"
#include "device_template.h"
#include "gateway_events.h"
#include "web_http.h"

static const char *TAG = "web_schema_api";

/* ── Helpers ────────────────────────────────────────────────────────── */

static const char *schema_state_name(device_schema_state_t state)
{
    switch (state) {
    case DEVICE_SCHEMA_STATE_READY:        return "ready";
    case DEVICE_SCHEMA_STATE_DISCOVERING:  return "discovering";
    case DEVICE_SCHEMA_STATE_UNSUPPORTED:  return "unsupported";
    case DEVICE_SCHEMA_STATE_ERROR:        return "error";
    default:                               return "unknown";
    }
}

static cJSON *tool_to_json(const device_schema_tool_t *tool)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return NULL;
    cJSON_AddStringToObject(obj, "command", tool->command);
    cJSON_AddStringToObject(obj, "label", tool->label);
    cJSON_AddStringToObject(obj, "unit", tool->unit);
    cJSON_AddNumberToObject(obj, "value_type", tool->value_type);
    cJSON_AddNumberToObject(obj, "flags", tool->flags);
    cJSON_AddNumberToObject(obj, "min_value", tool->min_value);
    cJSON_AddNumberToObject(obj, "max_value", tool->max_value);
    cJSON_AddNumberToObject(obj, "step", tool->step);
    return obj;
}

static cJSON *feature_to_json(const device_schema_feature_t *feature,
                               const device_schema_tool_t *tools,
                               size_t tool_count)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return NULL;

    cJSON_AddStringToObject(obj, "feature_id", feature->feature_id);
    cJSON_AddNumberToObject(obj, "feature_type", feature->feature_type);
    cJSON_AddNumberToObject(obj, "feature_schema_version",
                            feature->feature_schema_version);
    cJSON_AddNumberToObject(obj, "property_id", feature->property_id);
    cJSON_AddNumberToObject(obj, "writable_tool_index",
                            feature->writable_tool_index);

    /* Resolve template */
    const device_template_t *tpl = device_template_resolve(
        feature->feature_type, feature->feature_schema_version);
    cJSON *tpl_obj = cJSON_AddObjectToObject(obj, "template");
    if (tpl_obj != NULL) {
        if (tpl != NULL) {
            cJSON_AddStringToObject(tpl_obj, "semantic_name",
                                    device_template_semantic_name(tpl));
            cJSON_AddNumberToObject(tpl_obj, "primary_property",
                                    device_template_primary_property(tpl));
        } else {
            cJSON_AddNullToObject(tpl_obj, "semantic_name");
            cJSON_AddNullToObject(tpl_obj, "primary_property");
        }
    }

    /* Attach the write command name from the resolved tool */
    if (feature->writable_tool_index >= 0 &&
        (size_t)feature->writable_tool_index < tool_count) {
        cJSON_AddStringToObject(obj, "write_command",
                                tools[feature->writable_tool_index].command);
    }

    return obj;
}

/* ── GET /api/devices/schema ────────────────────────────────────────── */

static esp_err_t schema_get_handler(httpd_req_t *request)
{
    char query[128];
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        web_get_query_value(query, "device_id", device_id,
                            sizeof(device_id)) != ESP_OK ||
        device_id[0] == '\0') {
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }

    uint32_t base_seq = gateway_events_current_seq();

    device_schema_snapshot_t snapshot;
    esp_err_t error = device_schema_get(device_id, &snapshot);
    if (error == ESP_ERR_NOT_FOUND) {
        return web_send_api_error_code(request, "404 Not Found",
                                       "Device not found", "device_not_found");
    }
    if (error != ESP_OK) {
        return web_send_api_error_code(request, "500 Internal Server Error",
                                       "Failed to read schema",
                                       "internal_error");
    }

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(response, "device_id", snapshot.device_id);
    cJSON_AddStringToObject(response, "state",
                            schema_state_name(snapshot.state));
    cJSON_AddNumberToObject(response, "revision", snapshot.revision);
    cJSON_AddBoolToObject(response, "has_committed", snapshot.has_committed);
    cJSON_AddNumberToObject(response, "updated_at_ms", snapshot.updated_at_ms);

    /* Tools array */
    cJSON *tools_arr = cJSON_AddArrayToObject(response, "tools");
    if (tools_arr != NULL) {
        for (size_t i = 0; i < snapshot.tool_count; i++) {
            cJSON *tool_json = tool_to_json(&snapshot.tools[i]);
            if (tool_json != NULL) cJSON_AddItemToArray(tools_arr, tool_json);
        }
    }

    /* Features array with template + state enrichment */
    device_state_snapshot_t state_snapshot;
    device_state_snapshot(device_id, &state_snapshot);

    cJSON *features_arr = cJSON_AddArrayToObject(response, "features");
    if (features_arr != NULL) {
        for (size_t i = 0; i < snapshot.feature_count; i++) {
            cJSON *feat_json = feature_to_json(
                &snapshot.features[i], snapshot.tools, snapshot.tool_count);
            if (feat_json == NULL) continue;

            /* Attach runtime state for this feature */
            cJSON *state_obj = cJSON_AddObjectToObject(feat_json, "state");
            if (state_obj != NULL) {
                bool found = false;
                for (size_t s = 0; s < state_snapshot.count; s++) {
                    const device_state_entry_t *entry = &state_snapshot.entries[s];
                    if (strcmp(entry->feature_id,
                               snapshot.features[i].feature_id) == 0 &&
                        entry->property_id ==
                            snapshot.features[i].property_id) {
                        cJSON_AddBoolToObject(state_obj, "valid", entry->valid);
                        cJSON_AddBoolToObject(state_obj, "value_bool",
                                              entry->value_bool);
                        cJSON_AddNumberToObject(state_obj, "value_int",
                                                entry->value_int);
                        cJSON_AddNumberToObject(state_obj, "updated_at_ms",
                                                entry->updated_at_ms);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    cJSON_AddBoolToObject(state_obj, "valid", false);
                    cJSON_AddBoolToObject(state_obj, "value_bool", false);
                    cJSON_AddNumberToObject(state_obj, "value_int", 0);
                    cJSON_AddNumberToObject(state_obj, "updated_at_ms", 0);
                }
            }

            cJSON_AddItemToArray(features_arr, feat_json);
        }
    }

    char seq_text[16];
    snprintf(seq_text, sizeof(seq_text), "%" PRIu32, base_seq);
    httpd_resp_set_hdr(request, "X-Gateway-Event-Seq", seq_text);

    return web_send_json(request, response);
}

/* ── POST /api/devices/schema/refresh ───────────────────────────────── */

static esp_err_t schema_refresh_handler(httpd_req_t *request)
{
    char body[WEB_COMMAND_BODY_MAX_LEN];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) return web_send_body_error(request, body_status);
    const char *device_id = web_get_json_string(
        json, "device_id", GW_MSG_DEVICE_ID_LEN, true);
    if (device_id == NULL) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }

    /* Preflight: device exists? */
    device_schema_snapshot_t snapshot;
    esp_err_t snapshot_error = device_schema_get(device_id, &snapshot);
    if (snapshot_error == ESP_ERR_NOT_FOUND) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "404 Not Found",
                                       "Device not found", "device_not_found");
    }

    /* Preflight: BLE runtime ready? */
    ble_central_device_status_t ble_status;
    bool ble_ready =
        ble_central_get_device_status(device_id, &ble_status) == BLE_CENTRAL_OK &&
        ble_status.ready;
    if (!ble_ready) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "502 Bad Gateway",
                                       "Device is not BLE ready",
                                       "device_not_connected");
    }

    uint32_t generation = 0;
    esp_err_t error = device_schema_refresh(device_id, &generation);
    cJSON_Delete(json);
    if (error == ESP_ERR_NOT_FOUND) {
        return web_send_api_error_code(request, "404 Not Found",
                                       "Device not found", "device_not_found");
    }
    if (error == ESP_ERR_INVALID_STATE) {
        return web_send_api_error_code(request, "409 Conflict",
                                       "Schema operation already in progress",
                                       "device_busy");
    }
    if (error != ESP_OK) {
        return web_send_api_error_code(request, "503 Service Unavailable",
                                       "Schema refresh queue is full",
                                       "queue_full");
    }

    httpd_resp_set_status(request, "202 Accepted");
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "accepted", true);
    cJSON_AddStringToObject(response, "device_id", device_id);
    cJSON_AddNumberToObject(response, "generation", generation);
    return web_send_json(request, response);
}

/* ── Registration ───────────────────────────────────────────────────── */

esp_err_t web_device_schema_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        WEB_URI_INIT("/api/devices/schema", HTTP_GET, schema_get_handler),
        WEB_URI_INIT("/api/devices/schema/refresh", HTTP_POST, schema_refresh_handler),
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
