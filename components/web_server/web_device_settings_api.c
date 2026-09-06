#include "web_modules.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_settings.h"
#include "device_store.h"
#include "esp_log.h"
#include "web_http.h"

static const char *TAG = "web_dev_settings";

#define WEB_SETTINGS_BODY_MAX_LEN 1024

/* ── Settings type name mapping ─────────────────────────────────────── */

static const char *settings_type_name(uint8_t type)
{
    switch (type) {
    case DS_TYPE_BOOL:   return "boolean";
    case DS_TYPE_INT:    return "integer";
    case DS_TYPE_FLOAT:  return "number";
    case DS_TYPE_STRING: return "string";
    case DS_TYPE_ENUM:   return "enum";
    }
    return "unknown";
}

/* ── GET /api/devices/settings ──────────────────────────────────────── */

static esp_err_t settings_get_handler(httpd_req_t *request)
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

    /* Check device exists. */
    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        return web_send_api_error_code(request, "404 Not Found",
                                       "Device not found", "device_not_found");
    }

    /* Check settings schema state. */
    ds_schema_state_t schema_state;
    (void)device_settings_get_state(device_id, &schema_state);

    if (schema_state == DS_SCHEMA_UNSUPPORTED ||
        schema_state == DS_SCHEMA_UNKNOWN) {
        return web_send_api_error_code(request, "404 Not Found",
                                       "Settings not supported",
                                       "settings_unsupported");
    }

    if (schema_state == DS_SCHEMA_DISCOVERING) {
        return web_send_api_error_code(request, "503 Service Unavailable",
                                       "Settings discovering",
                                       "settings_discovering");
    }

    if (schema_state != DS_SCHEMA_READY) {
        return web_send_api_error_code(request, "503 Service Unavailable",
                                       "Settings not ready",
                                       "settings_not_ready");
    }

    /* Acquire snapshots. */
    const ds_schema_t *schema = device_settings_schema_acquire(device_id);
    const ds_values_t *values = device_settings_values_acquire(device_id);

    if (schema == NULL) {
        if (values != NULL) device_settings_values_release(values);
        return web_send_api_error_code(request, "503 Service Unavailable",
                                       "Schema unavailable",
                                       "schema_unavailable");
    }

    /* Build response. */
    cJSON *root = cJSON_CreateObject();
    cJSON *settings_arr = cJSON_CreateArray();
    if (root == NULL || settings_arr == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(settings_arr);
        device_settings_schema_release(schema);
        if (values != NULL) device_settings_values_release(values);
        return web_send_api_error(request, "500 Internal Server Error",
                                  "Out of memory");
    }

    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "state",
                            device_settings_schema_state_name(schema_state));
    cJSON_AddNumberToObject(root, "schema_revision", schema->schema_revision);
    cJSON_AddNumberToObject(root, "config_revision",
                            values != NULL ? values->config_revision : 0);

    /* Build settings array. */
    for (uint16_t i = 0; i < schema->setting_count; i++) {
        const ds_setting_desc_t *desc = &schema->descriptors[i];
        const char *id = ds_string_pool_get(&schema->strings, desc->id_off);
        const char *title = ds_string_pool_get(&schema->strings,
                                               desc->title_off);
        const char *group = ds_string_pool_get(&schema->strings,
                                               desc->group_off);
        const char *unit = ds_string_pool_get(&schema->strings, desc->unit_off);

        if (id == NULL || id[0] == '\0') continue;

        /* Secret settings: never return plaintext. */
        if (desc->flags & DS_FLAG_SECRET) {
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) continue;
            cJSON_AddStringToObject(item, "id", id);
            cJSON_AddStringToObject(item, "type", "secret");

            /* Check if a value is configured. */
            bool configured = false;
            if (values != NULL) {
                for (uint16_t v = 0; v < values->value_count; v++) {
                    const char *vid = ds_string_pool_get(
                        &values->string_pool, values->values[v].id_off);
                    if (vid != NULL && strcmp(vid, id) == 0) {
                        configured = values->values[v].has_value;
                        break;
                    }
                }
            }
            cJSON_AddBoolToObject(item, "configured", configured);
            cJSON_AddItemToArray(settings_arr, item);
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        if (item == NULL) continue;

        cJSON_AddStringToObject(item, "id", id);
        if (title != NULL && title[0] != '\0') {
            cJSON_AddStringToObject(item, "title", title);
        }
        if (group != NULL && group[0] != '\0') {
            cJSON_AddStringToObject(item, "group", group);
        }
        cJSON_AddStringToObject(item, "type", settings_type_name(desc->type));
        cJSON_AddBoolToObject(item, "readonly",
                              !(desc->flags & DS_FLAG_WRITABLE));

        if (unit != NULL && unit[0] != '\0') {
            cJSON_AddStringToObject(item, "unit", unit);
        }

        /* Range metadata for numeric types. */
        if (desc->type == DS_TYPE_INT || desc->type == DS_TYPE_FLOAT) {
            cJSON_AddNumberToObject(item, "minimum", desc->min_value);
            cJSON_AddNumberToObject(item, "maximum", desc->max_value);
            cJSON_AddNumberToObject(item, "step", desc->step);
        }

        /* Find matching value. */
        if (values != NULL) {
            for (uint16_t v = 0; v < values->value_count; v++) {
                const char *vid = ds_string_pool_get(
                    &values->string_pool, values->values[v].id_off);
                if (vid != NULL && strcmp(vid, id) == 0 &&
                    values->values[v].has_value) {
                    const ds_value_entry_t *ve = &values->values[v];
                    switch (ve->type) {
                    case DS_TYPE_BOOL:
                        cJSON_AddBoolToObject(item, "value", ve->bool_val);
                        break;
                    case DS_TYPE_INT:
                    case DS_TYPE_ENUM:
                        cJSON_AddNumberToObject(item, "value", ve->int_val);
                        break;
                    case DS_TYPE_FLOAT:
                        cJSON_AddNumberToObject(item, "value",
                                                (double)ve->float_val);
                        break;
                    case DS_TYPE_STRING: {
                        const char *sv = ds_string_pool_get(
                            &values->string_pool, ve->string_off);
                        cJSON_AddStringToObject(item, "value",
                                                sv != NULL ? sv : "");
                        break;
                    }
                    }
                    break;
                }
            }
        }

        cJSON_AddItemToArray(settings_arr, item);
    }

    cJSON_AddItemToObject(root, "settings", settings_arr);

    /* Release snapshots. */
    device_settings_schema_release(schema);
    if (values != NULL) device_settings_values_release(values);

    return web_send_json(request, root);
}

/* ── PUT /api/devices/settings ──────────────────────────────────────── */

static esp_err_t settings_put_handler(httpd_req_t *request)
{
    char body[WEB_SETTINGS_BODY_MAX_LEN];
    web_body_status_t body_status;
    cJSON *json = web_parse_request_json(request, body, sizeof(body),
                                         &body_status);
    if (json == NULL) return web_send_body_error(request, body_status);

    /* Extract device_id. */
    const char *device_id = web_get_json_string(json, "device_id",
                                                GW_MSG_DEVICE_ID_LEN, true);
    if (device_id == NULL || device_id[0] == '\0') {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing device_id", "invalid_request");
    }

    /* Check device exists. */
    device_entry_t entry;
    if (device_store_get(device_id, &entry) != DEVICE_STORE_OK) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "404 Not Found",
                                       "Device not found", "device_not_found");
    }

    /* Check schema is ready. */
    ds_schema_state_t schema_state;
    (void)device_settings_get_state(device_id, &schema_state);
    if (schema_state != DS_SCHEMA_READY) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "503 Service Unavailable",
                                       "Settings not ready",
                                       "settings_not_ready");
    }

    /* Extract expected_revision. */
    cJSON *rev_json = cJSON_GetObjectItem(json, "expected_revision");
    if (!cJSON_IsNumber(rev_json)) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing expected_revision",
                                       "invalid_request");
    }
    uint32_t expected_rev = (uint32_t)rev_json->valuedouble;

    /* Extract changes array. */
    cJSON *changes_json = cJSON_GetObjectItem(json, "changes");
    if (!cJSON_IsArray(changes_json)) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Missing changes array",
                                       "invalid_request");
    }

    int change_count = cJSON_GetArraySize(changes_json);
    if (change_count <= 0 || change_count > DS_TX_MAX_CHANGES) {
        cJSON_Delete(json);
        return web_send_api_error_code(request, "400 Bad Request",
                                       "Invalid changes count",
                                       "invalid_request");
    }

    /* Parse changes into ds_change_request_t array. */
    ds_change_request_t changes[DS_TX_MAX_CHANGES];
    memset(changes, 0, sizeof(changes));
    int valid_count = 0;

    for (int i = 0; i < change_count; i++) {
        cJSON *ch = cJSON_GetArrayItem(changes_json, i);
        if (ch == NULL) continue;

        const char *id = web_get_json_string(ch, "id",
                                             GW_SETTINGS_CBOR_MAX_ID_LEN,
                                             true);
        if (id == NULL || id[0] == '\0') continue;

        /* Check for secret_action. */
        cJSON *secret_action = cJSON_GetObjectItem(ch, "secret_action");
        if (cJSON_IsString(secret_action)) {
            const char *action = secret_action->valuestring;
            if (strcmp(action, "keep") == 0) {
                continue;  /* Skip — keep current value. */
            }
            /* "clear" and other actions not yet supported. */
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                           "Unsupported secret_action",
                                           "invalid_secret_action");
        }

        strlcpy(changes[valid_count].setting_id, id,
                sizeof(changes[valid_count].setting_id));

        /* Determine type and value from schema. */
        const ds_schema_t *schema =
            device_settings_schema_acquire(device_id);
        if (schema == NULL) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "503 Service Unavailable",
                                           "Schema unavailable",
                                           "schema_unavailable");
        }

        bool found = false;
        for (uint16_t s = 0; s < schema->setting_count; s++) {
            const char *sid = ds_string_pool_get(&schema->strings,
                                                 schema->descriptors[s].id_off);
            if (sid != NULL && strcmp(sid, id) == 0) {
                const ds_setting_desc_t *desc = &schema->descriptors[s];
                changes[valid_count].type = desc->type;

                cJSON *val = cJSON_GetObjectItem(ch, "value");
                switch (desc->type) {
                case DS_TYPE_BOOL:
                    changes[valid_count].bool_val =
                        cJSON_IsBool(val) && cJSON_IsTrue(val);
                    break;
                case DS_TYPE_INT:
                    changes[valid_count].int_val =
                        cJSON_IsNumber(val) ? val->valueint : 0;
                    break;
                case DS_TYPE_FLOAT:
                    changes[valid_count].float_val =
                        cJSON_IsNumber(val) ? (float)val->valuedouble : 0.0f;
                    break;
                case DS_TYPE_ENUM:
                    changes[valid_count].enum_val =
                        cJSON_IsNumber(val) ? val->valueint : 0;
                    break;
                    case DS_TYPE_STRING:
                    if (cJSON_IsString(val)) {
                        strlcpy(changes[valid_count].string_val.str,
                                val->valuestring,
                                sizeof(changes[valid_count].string_val.str));
                    }
                    break;
                }

                found = true;
                break;
            }
        }

        device_settings_schema_release(schema);

        if (!found) {
            cJSON_Delete(json);
            return web_send_api_error_code(request, "400 Bad Request",
                                           "Unknown setting id",
                                           "invalid_setting");
        }

        valid_count++;
    }

    cJSON_Delete(json);

    if (valid_count == 0) {
        return web_send_api_error_code(request, "400 Bad Request",
                                       "No valid changes", "no_changes");
    }

    /* Submit async save. */
    esp_err_t err = device_settings_save(device_id, changes,
                                         (uint16_t)valid_count,
                                         expected_rev, NULL, NULL);
    if (err == ESP_ERR_INVALID_STATE) {
        return web_send_api_error_code(request, "409 Conflict",
                                       "Active transaction in progress",
                                       "active_transaction");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return web_send_api_error_code(request, "422 Unprocessable Entity",
                                       "Validation failed",
                                       "validation_failed");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] save failed: %s", device_id, esp_err_to_name(err));
        return web_send_api_error_code(request, "500 Internal Server Error",
                                       "Save failed", "internal_error");
    }

    /* Return 202 Accepted. */
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return web_send_api_error(request, "500 Internal Server Error",
                                  "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddStringToObject(resp, "operation_id", device_id);
    cJSON_AddStringToObject(resp, "state", "queued");

    httpd_resp_set_status(request, "202 Accepted");
    return web_send_json(request, resp);
}

/* ── GET /api/devices/settings/operations ───────────────────────────── */

static esp_err_t operations_get_handler(httpd_req_t *request)
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

    bool active = false;
    ds_tx_state_t state = DS_TX_IDLE;
    ds_tx_result_t last_result = DS_TX_RESULT_OK;
    (void)device_settings_tx_get_status(device_id, &active, &state,
                                        &last_result);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return web_send_api_error(request, "500 Internal Server Error",
                                  "Out of memory");
    }

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddBoolToObject(root, "active", active);
    cJSON_AddStringToObject(root, "state",
                            active ? device_settings_tx_state_name(state)
                                   : "idle");

    if (!active) {
        const char *err_name = device_settings_tx_result_name(last_result);
        if (err_name != NULL) {
            cJSON_AddStringToObject(root, "error", err_name);
        } else {
            cJSON_AddNullToObject(root, "error");
        }
    } else {
        cJSON_AddNullToObject(root, "error");
    }

    return web_send_json(request, root);
}

/* ── Registration ───────────────────────────────────────────────────── */

esp_err_t web_device_settings_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        WEB_URI_INIT("/api/devices/settings", HTTP_GET,
                     settings_get_handler),
        WEB_URI_INIT("/api/devices/settings", HTTP_PUT,
                     settings_put_handler),
        WEB_URI_INIT("/api/devices/settings/operations", HTTP_GET,
                     operations_get_handler),
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
