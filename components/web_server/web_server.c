#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ble_central.h"
#include "command_dispatcher.h"
#include "device_store.h"
#include "log_buffer.h"
#include "web_server.h"
#include "wifi_prov.h"

static const char *TAG = "web_server";

#define REQUEST_BODY_MAX_LEN 1024
#define BLE_SCAN_CACHE_SIZE   20
#define BLE_SCAN_DURATION_MS  6000

typedef struct {
    ble_scan_result_t result;
    int64_t last_seen_ms;
} scan_cache_entry_t;

static scan_cache_entry_t s_scan_cache[BLE_SCAN_CACHE_SIZE];
static int s_scan_cache_count;
static SemaphoreHandle_t s_scan_mutex;
static volatile bool s_scan_stop_task_active;

static const char INDEX_HTML[] =
"<!doctype html><html lang='vi'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 BLE Gateway</title><style>"
":root{font-family:system-ui,sans-serif;color:#172033;background:#f4f7fb}body{margin:0 auto;max-width:1050px;padding:24px}"
"h1{margin:0 0 8px}.muted{color:#667085}.card{background:white;border:1px solid #d9e1ec;border-radius:12px;padding:18px;margin:16px 0;box-shadow:0 4px 14px #21354710}"
"input,select,button{box-sizing:border-box;padding:9px 11px;margin:4px;border:1px solid #b8c4d4;border-radius:7px;background:white}button{cursor:pointer;background:#1769e0;color:white;border-color:#1769e0}button.secondary{background:white;color:#1769e0}button.danger{background:#c9362b;border-color:#c9362b}"
"table{border-collapse:collapse;width:100%}th,td{border-bottom:1px solid #e3e8ef;padding:9px;text-align:left}#logs{height:220px;overflow:auto;background:#101828;color:#a6f4c5;padding:12px;border-radius:8px;white-space:pre-wrap;font:12px ui-monospace,monospace}.ok{color:#087443}.error{color:#b42318}.row{display:flex;flex-wrap:wrap;align-items:center}.grow{flex:1;min-width:180px}"
"</style></head><body><h1>ESP32 BLE Gateway</h1><div id='summary' class='muted'>Đang tải trạng thái…</div>"
"<section id='wifiCard' class='card' hidden><h2>Cấu hình Wi-Fi</h2><p>Chọn mạng LAN. Gateway chỉ lưu mật khẩu sau khi kết nối và nhận IP thành công.</p>"
"<div class='row'><input class='grow' id='wifiSsid' list='wifiList' placeholder='SSID'><datalist id='wifiList'></datalist><button class='secondary' onclick='scanWifi()'>Quét Wi-Fi</button></div>"
"<div class='row'><input class='grow' id='wifiPass' type='password' placeholder='Mật khẩu'><button onclick='saveWifi()'>Kiểm tra và kết nối</button></div><div id='wifiMessage'></div></section>"
"<main id='gatewayPanel' hidden><section class='card'><div class='row'><h2 class='grow'>Thiết bị BLE khả dụng</h2><button onclick='startBleScan()'>Quét BLE 6 giây</button></div><div id='scanState' class='muted'></div><table><thead><tr><th>Thiết bị</th><th>MAC</th><th>RSSI</th><th></th></tr></thead><tbody id='scanRows'></tbody></table></section>"
"<section class='card'><h2>Thêm thiết bị</h2><div class='row'><input id='deviceId' placeholder='device_id'><input id='deviceName' placeholder='Tên'><input id='deviceType' value='generic' placeholder='Loại'><input id='deviceAddr' placeholder='AA:BB:CC:DD:EE:FF'><input id='deviceAddrType' type='number' value='0' min='0' max='255'><button onclick='addDevice()'>Thêm</button></div><div id='deviceMessage'></div></section>"
"<section class='card'><h2>Danh sách thiết bị</h2><table><thead><tr><th>ID</th><th>Tên</th><th>Loại</th><th>BLE</th><th>Thao tác</th></tr></thead><tbody id='deviceRows'></tbody></table></section>"
"<section class='card'><h2>Log gần đây</h2><div id='logs'></div></section></main>"
"<script>const $=id=>document.getElementById(id);let busy=false;function note(id,text,ok){let e=$(id);e.textContent=text;e.className=ok?'ok':'error'}"
"async function jsonFetch(url,options){let r=await fetch(url,options);let data=await r.json();if(!r.ok||data.success===false)throw new Error(data.message||('HTTP '+r.status));return data}"
"async function status(){try{let s=await jsonFetch('/api/status');$('summary').textContent=`Wi-Fi: ${s.wifi_state} · IP: ${s.ip} · BLE: ${s.connected_count}/${s.device_count} thiết bị · heap ${s.free_heap} B`;$('wifiCard').hidden=!s.provisioning;$('gatewayPanel').hidden=s.provisioning;if(s.provisioning&&$('wifiList').children.length===0&&!busy)scanWifi()}catch(e){$('summary').textContent=e.message}}"
"async function scanWifi(){if(busy)return;busy=true;note('wifiMessage','Đang quét…',true);try{let d=await jsonFetch('/api/wifi/scan');let list=$('wifiList');list.replaceChildren();for(let n of d.networks){let o=document.createElement('option');o.value=n.ssid;o.label=`${n.rssi} dBm${n.secure?' · bảo mật':''}`;list.appendChild(o)}note('wifiMessage',`Tìm thấy ${d.networks.length} mạng.`,true)}catch(e){note('wifiMessage',e.message,false)}finally{busy=false}}"
"async function saveWifi(){let ssid=$('wifiSsid').value.trim(),password=$('wifiPass').value;if(!ssid)return note('wifiMessage','Vui lòng nhập SSID.',false);busy=true;note('wifiMessage','Đang kiểm tra kết nối…',true);try{await jsonFetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,password})});note('wifiMessage','Đã kết nối và lưu. Gateway đang chuyển sang mạng LAN…',true)}catch(e){busy=false;note('wifiMessage',e.message,false)}}"
"function td(row,text){let c=document.createElement('td');c.textContent=text;row.appendChild(c);return c}function button(text,fn,kind){let b=document.createElement('button');b.textContent=text;b.onclick=fn;if(kind)b.className=kind;return b}"
"async function startBleScan(){try{await jsonFetch('/api/ble/scan',{method:'POST'});$('scanState').textContent='Đang quét…';setTimeout(loadBleScan,800)}catch(e){$('scanState').textContent=e.message}}"
"async function loadBleScan(){if($('gatewayPanel').hidden)return;try{let d=await jsonFetch('/api/ble/scan'),body=$('scanRows');body.replaceChildren();$('scanState').textContent=d.scanning?'Đang quét…':`Tìm thấy ${d.devices.length} thiết bị`;for(let x of d.devices){let r=document.createElement('tr');td(r,x.name||'(không tên)');td(r,x.ble_addr);td(r,String(x.rssi));let a=td(r,'');a.appendChild(button('Chọn',()=>{$('deviceId').value=x.ble_addr;$('deviceName').value=x.name||x.ble_addr;$('deviceAddr').value=x.ble_addr;$('deviceAddrType').value=x.addr_type})) ;body.appendChild(r)}if(d.scanning)setTimeout(loadBleScan,1000)}catch(e){$('scanState').textContent=e.message}}"
"async function addDevice(){let payload={device_id:$('deviceId').value.trim(),name:$('deviceName').value.trim(),type:$('deviceType').value.trim()||'generic'};let addr=$('deviceAddr').value.trim();if(addr){payload.ble_addr=addr;payload.ble_addr_type=Number($('deviceAddrType').value||0)}try{let d=await jsonFetch('/api/devices',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});note('deviceMessage',d.message,true);loadDevices()}catch(e){note('deviceMessage',e.message,false)}}"
"async function editDevice(d){let name=prompt('Tên mới',d.name);if(name===null)return;let type=prompt('Loại mới',d.type);if(type===null)return;try{await jsonFetch('/api/devices',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({device_id:d.device_id,name,type})});loadDevices()}catch(e){note('deviceMessage',e.message,false)}}"
"async function deleteDevice(id){if(!confirm(`Xóa ${id}?`))return;try{await jsonFetch('/api/devices?device_id='+encodeURIComponent(id),{method:'DELETE'});loadDevices()}catch(e){note('deviceMessage',e.message,false)}}"
"async function sendCommand(id,command,boolValue){try{let d=await jsonFetch('/api/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({device_id:id,command,bool_value:boolValue})});note('deviceMessage',d.message,true);loadLogs()}catch(e){note('deviceMessage',e.message,false)}}"
"async function customCommand(id){let command=prompt('Tên lệnh','toggle');if(command)sendCommand(id,command,true)}"
"async function loadDevices(){if($('gatewayPanel').hidden)return;try{let list=await jsonFetch('/api/devices'),body=$('deviceRows');body.replaceChildren();for(let d of list){let r=document.createElement('tr');td(r,d.device_id);td(r,d.name);td(r,d.type);td(r,d.connected?'Đã kết nối':(d.ble_addr||'Chưa có MAC'));let a=td(r,'');a.append(button('Toggle',()=>sendCommand(d.device_id,'toggle',true)));a.append(button('Lệnh…',()=>customCommand(d.device_id),'secondary'));a.append(button('Sửa',()=>editDevice(d),'secondary'));a.append(button('Xóa',()=>deleteDevice(d.device_id),'danger'));body.appendChild(r)}}catch(e){note('deviceMessage',e.message,false)}}"
"async function loadLogs(){if($('gatewayPanel').hidden)return;try{let logs=await jsonFetch('/api/logs');$('logs').textContent=logs.map(x=>`[${x.timestamp_ms}] ${x.text}`).join('\\n')}catch(e){$('logs').textContent=e.message}}"
"async function refresh(){await status();if(!$('gatewayPanel').hidden){await Promise.all([loadDevices(),loadLogs()])}}setInterval(refresh,3000);refresh();</script></body></html>";

static esp_err_t send_json(httpd_req_t *request, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (text == NULL) {
        httpd_resp_send_500(request);
        return ESP_FAIL;
    }
    httpd_resp_set_type(request, "application/json");
    esp_err_t result = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return result;
}

static esp_err_t send_api_error(httpd_req_t *request, const char *status,
                                const char *message)
{
    httpd_resp_set_status(request, status);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "message", message);
    return send_json(request, json);
}

static int read_request_body(httpd_req_t *request, char *buffer, size_t capacity)
{
    if (request->content_len <= 0 || request->content_len >= (int)capacity) return -1;
    int total = 0;
    while (total < request->content_len) {
        int received = httpd_req_recv(request, buffer + total,
                                      request->content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0) return -1;
        total += received;
    }
    buffer[total] = '\0';
    return total;
}

static cJSON *parse_request_json(httpd_req_t *request, char *buffer, size_t capacity)
{
    if (read_request_body(request, buffer, capacity) < 0) return NULL;
    cJSON *json = cJSON_Parse(buffer);
    return cJSON_IsObject(json) ? json : (cJSON_Delete(json), (cJSON *)NULL);
}

static const char *get_json_string(const cJSON *object, const char *key,
                                   size_t max_length, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (item == NULL && !required) return NULL;
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        (required && item->valuestring[0] == '\0') ||
        strnlen(item->valuestring, max_length) >= max_length) {
        return NULL;
    }
    return item->valuestring;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
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
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[5], address[4], address[3], address[2], address[1], address[0]);
}

static esp_err_t send_dispatch_result(httpd_req_t *request,
                                      const dispatch_result_t *result)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", result->success != 0);
    cJSON_AddStringToObject(json, "message", result->message);
    cJSON *data = cJSON_Parse(result->message);
    if (data != NULL) cJSON_AddItemToObject(json, "data", data);
    return send_json(request, json);
}

static esp_err_t index_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_redirect_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t devices_get_handler(httpd_req_t *request)
{
    gw_message_t message = {.protocol_version = GW_PROTOCOL_VERSION};
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, "list_devices", sizeof(message.command));
    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    if (!result.success) {
        return send_api_error(request, "500 Internal Server Error", result.message);
    }
    cJSON *array = cJSON_Parse(result.message);
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(array);
        return send_api_error(request, "500 Internal Server Error",
                              "Dispatcher returned an invalid device list");
    }
    return send_json(request, array);
}

static int fill_device_message(const cJSON *json, gw_message_t *message,
                               const char *command, bool require_metadata)
{
    memset(message, 0, sizeof(*message));
    message->protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(message->type, "gateway_command", sizeof(message->type));
    strlcpy(message->command, command, sizeof(message->command));

    const char *device_id = get_json_string(json, "device_id",
                                            sizeof(message->device_id), true);
    if (device_id == NULL) return -1;
    strlcpy(message->device_id, device_id, sizeof(message->device_id));
    message->has_device_id = true;

    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(json, "name");
    if (name_item != NULL) {
        const char *name = get_json_string(json, "name", sizeof(message->name), true);
        if (name == NULL) return -1;
        strlcpy(message->name, name, sizeof(message->name));
    }
    const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(json, "type");
    if (type_item != NULL) {
        const char *type = get_json_string(json, "type", sizeof(message->device_type), true);
        if (type == NULL) return -1;
        strlcpy(message->device_type, type, sizeof(message->device_type));
    }
    if (require_metadata && message->name[0] == '\0') {
        strlcpy(message->name, message->device_id, sizeof(message->name));
    }
    if (require_metadata && message->device_type[0] == '\0') {
        strlcpy(message->device_type, "generic", sizeof(message->device_type));
    }

    const cJSON *address_item = cJSON_GetObjectItemCaseSensitive(json, "ble_addr");
    if (address_item != NULL) {
        const char *address = get_json_string(json, "ble_addr", 18, true);
        if (address == NULL || parse_ble_addr(address, message->ble_addr) != 0) return -1;
        const cJSON *address_type = cJSON_GetObjectItemCaseSensitive(json, "ble_addr_type");
        if (address_type != NULL && (!cJSON_IsNumber(address_type) ||
            address_type->valueint < 0 || address_type->valueint > UINT8_MAX ||
            address_type->valuedouble != (double)address_type->valueint)) return -1;
        message->ble_addr_type = address_type != NULL ? (uint8_t)address_type->valueint : 0;
        message->has_ble_addr = true;
    }
    return 0;
}

static esp_err_t devices_write_handler(httpd_req_t *request)
{
    char body[REQUEST_BODY_MAX_LEN];
    cJSON *json = parse_request_json(request, body, sizeof(body));
    if (json == NULL) return send_api_error(request, "400 Bad Request", "Invalid JSON body");

    const char *command = request->method == HTTP_POST ? "add_device" : "edit_device";
    gw_message_t message;
    int parse_result = fill_device_message(json, &message, command,
                                           request->method == HTTP_POST);
    cJSON_Delete(json);
    if (parse_result != 0) {
        return send_api_error(request, "400 Bad Request", "Invalid device fields");
    }
    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    return send_dispatch_result(request, &result);
}

static esp_err_t devices_delete_handler(httpd_req_t *request)
{
    char query[128];
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "device_id", device_id, sizeof(device_id)) != ESP_OK ||
        device_id[0] == '\0') {
        return send_api_error(request, "400 Bad Request", "Missing device_id");
    }

    gw_message_t message = {.protocol_version = GW_PROTOCOL_VERSION, .has_device_id = true};
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, "delete_device", sizeof(message.command));
    strlcpy(message.device_id, device_id, sizeof(message.device_id));
    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    return send_dispatch_result(request, &result);
}

static esp_err_t command_post_handler(httpd_req_t *request)
{
    char body[REQUEST_BODY_MAX_LEN];
    cJSON *json = parse_request_json(request, body, sizeof(body));
    if (json == NULL) return send_api_error(request, "400 Bad Request", "Invalid JSON body");

    const char *device_id = get_json_string(json, "device_id", GW_MSG_DEVICE_ID_LEN, true);
    const char *command = get_json_string(json, "command", GW_MSG_COMMAND_LEN, true);
    if (device_id == NULL || command == NULL) {
        cJSON_Delete(json);
        return send_api_error(request, "400 Bad Request", "device_id and command are required");
    }

    gw_message_t message = {.protocol_version = GW_PROTOCOL_VERSION, .has_device_id = true};
    strlcpy(message.type, "device_command", sizeof(message.type));
    strlcpy(message.device_id, device_id, sizeof(message.device_id));
    strlcpy(message.command, command, sizeof(message.command));

    const cJSON *int_value = cJSON_GetObjectItemCaseSensitive(json, "int_value");
    if (int_value != NULL) {
        if (!cJSON_IsNumber(int_value) || int_value->valuedouble != (double)int_value->valueint) {
            cJSON_Delete(json);
            return send_api_error(request, "400 Bad Request", "int_value must be an integer");
        }
        message.int_value = int_value->valueint;
    }
    const cJSON *bool_value = cJSON_GetObjectItemCaseSensitive(json, "bool_value");
    if (bool_value != NULL) {
        if (!cJSON_IsBool(bool_value)) {
            cJSON_Delete(json);
            return send_api_error(request, "400 Bad Request", "bool_value must be boolean");
        }
        message.bool_value = cJSON_IsTrue(bool_value);
    }
    cJSON_Delete(json);

    dispatch_result_t result;
    command_dispatcher_handle(&message, &result);
    return send_dispatch_result(request, &result);
}

static esp_err_t logs_get_handler(httpd_req_t *request)
{
    log_entry_t entries[LOG_BUFFER_CAPACITY];
    int count = log_buffer_get_all(entries);
    if (count < 0) return send_api_error(request, "500 Internal Server Error", "Could not read logs");
    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "text", entries[i].text);
        cJSON_AddNumberToObject(item, "timestamp_ms", entries[i].timestamp_ms);
        cJSON_AddItemToArray(array, item);
    }
    return send_json(request, array);
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    device_entry_t devices[DEVICE_STORE_MAX_DEVICES];
    int count = device_store_snapshot(devices, DEVICE_STORE_MAX_DEVICES);
    if (count < 0) count = 0;
    int connected = 0;
    for (int i = 0; i < count; i++) connected += devices[i].connected != 0;
    char ip[16];
    wifi_prov_get_ip(ip, sizeof(ip));

    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "device_count", count);
    cJSON_AddNumberToObject(json, "connected_count", connected);
    cJSON_AddNumberToObject(json, "ble_link_count", ble_central_active_count());
    cJSON_AddStringToObject(json, "ip", ip);
    cJSON_AddBoolToObject(json, "wifi_connected", wifi_prov_is_connected());
    cJSON_AddBoolToObject(json, "provisioning", wifi_prov_is_provisioning());
    cJSON_AddStringToObject(json, "wifi_state",
                            wifi_prov_state_name(wifi_prov_get_state()));
    cJSON_AddNumberToObject(json, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(json, "uptime_ms", esp_timer_get_time() / 1000);
    return send_json(request, json);
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *request)
{
    wifi_prov_ap_record_t records[WIFI_PROV_MAX_SCAN_RESULTS];
    size_t count = 0;
    int rc = wifi_prov_scan(records, WIFI_PROV_MAX_SCAN_RESULTS, &count);
    if (rc != 0) {
        return send_api_error(request, rc == -2 ? "409 Conflict" : "500 Internal Server Error",
                              rc == -2 ? "Gateway is not provisioning" : "Wi-Fi scan failed");
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON *networks = cJSON_AddArrayToObject(json, "networks");
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddBoolToObject(item, "secure", records[i].authmode != 0);
        cJSON_AddItemToArray(networks, item);
    }
    return send_json(request, json);
}

static esp_err_t wifi_post_handler(httpd_req_t *request)
{
    char body[REQUEST_BODY_MAX_LEN];
    cJSON *json = parse_request_json(request, body, sizeof(body));
    if (json == NULL) return send_api_error(request, "400 Bad Request", "Invalid JSON body");
    const char *ssid = get_json_string(json, "ssid", 33, true);
    const char *password = get_json_string(json, "password", 65, false);
    if (ssid == NULL || (cJSON_GetObjectItemCaseSensitive(json, "password") != NULL &&
                         password == NULL)) {
        cJSON_Delete(json);
        return send_api_error(request, "400 Bad Request", "Invalid SSID or password");
    }
    char ssid_copy[33];
    char password_copy[65];
    strlcpy(ssid_copy, ssid, sizeof(ssid_copy));
    strlcpy(password_copy, password != NULL ? password : "", sizeof(password_copy));
    cJSON_Delete(json);

    int rc = wifi_prov_test_and_save(ssid_copy, password_copy);
    if (rc != 0) {
        const char *message = rc == -2 ? "Gateway is not provisioning"
                              : rc == -5 ? "Connection failed; check SSID and password"
                                         : "Could not verify Wi-Fi configuration";
        return send_api_error(request, rc == -2 ? "409 Conflict" : "400 Bad Request", message);
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Wi-Fi verified and saved");
    esp_err_t send_result = send_json(request, response);
    wifi_prov_schedule_sta_only(1500);
    return send_result;
}

static void on_ble_scan_result(const ble_scan_result_t *result)
{
    if (result == NULL || s_scan_mutex == NULL ||
        xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    int index = -1;
    for (int i = 0; i < s_scan_cache_count; i++) {
        if (s_scan_cache[i].result.addr_type == result->addr_type &&
            memcmp(s_scan_cache[i].result.addr, result->addr, sizeof(result->addr)) == 0) {
            index = i;
            break;
        }
    }
    if (index < 0 && s_scan_cache_count < BLE_SCAN_CACHE_SIZE) index = s_scan_cache_count++;
    if (index >= 0) {
        s_scan_cache[index].result = *result;
        s_scan_cache[index].last_seen_ms = esp_timer_get_time() / 1000;
    }
    xSemaphoreGive(s_scan_mutex);
}

static void ble_scan_stop_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(BLE_SCAN_DURATION_MS));
    ble_central_scan_stop();
    s_scan_stop_task_active = false;
    vTaskDelete(NULL);
}

static esp_err_t ble_scan_post_handler(httpd_req_t *request)
{
    if (ble_central_is_scanning()) {
        cJSON *json = cJSON_CreateObject();
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddBoolToObject(json, "scanning", true);
        return send_json(request, json);
    }
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        memset(s_scan_cache, 0, sizeof(s_scan_cache));
        s_scan_cache_count = 0;
        xSemaphoreGive(s_scan_mutex);
    }
    if (ble_central_scan_start(on_ble_scan_result) != 0) {
        return send_api_error(request, "409 Conflict", "BLE host is not ready or busy");
    }
    if (!s_scan_stop_task_active) {
        s_scan_stop_task_active = xTaskCreate(ble_scan_stop_task, "ble_scan_stop", 2048,
                                              NULL, 4, NULL) == pdPASS;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddBoolToObject(json, "scanning", true);
    return send_json(request, json);
}

static esp_err_t ble_scan_get_handler(httpd_req_t *request)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddBoolToObject(json, "scanning", ble_central_is_scanning());
    cJSON *devices = cJSON_AddArrayToObject(json, "devices");
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        for (int i = 0; i < s_scan_cache_count; i++) {
            char address[18];
            format_ble_addr(s_scan_cache[i].result.addr, address);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", s_scan_cache[i].result.name);
            cJSON_AddStringToObject(item, "ble_addr", address);
            cJSON_AddNumberToObject(item, "addr_type", s_scan_cache[i].result.addr_type);
            cJSON_AddNumberToObject(item, "rssi", s_scan_cache[i].result.rssi);
            cJSON_AddItemToArray(devices, item);
        }
        xSemaphoreGive(s_scan_mutex);
    }
    return send_json(request, json);
}

httpd_handle_t web_server_start(void)
{
    if (s_scan_mutex == NULL) s_scan_mutex = xSemaphoreCreateMutex();
    if (s_scan_mutex == NULL) return NULL;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size = 12288;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) return NULL;

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get_handler},
        {.uri = "/api/devices", .method = HTTP_GET, .handler = devices_get_handler},
        {.uri = "/api/devices", .method = HTTP_POST, .handler = devices_write_handler},
        {.uri = "/api/devices", .method = HTTP_PUT, .handler = devices_write_handler},
        {.uri = "/api/devices", .method = HTTP_DELETE, .handler = devices_delete_handler},
        {.uri = "/api/command", .method = HTTP_POST, .handler = command_post_handler},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = logs_get_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get_handler},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler},
        {.uri = "/api/ble/scan", .method = HTTP_GET, .handler = ble_scan_get_handler},
        {.uri = "/api/ble/scan", .method = HTTP_POST, .handler = ble_scan_post_handler},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect_handler},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_handler},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect_handler},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect_handler},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t error = httpd_register_uri_handler(server, &routes[i]);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Could not register %s: %s", routes[i].uri,
                     esp_err_to_name(error));
            httpd_stop(server);
            return NULL;
        }
    }
    ESP_LOGI(TAG, "Web server started with %u routes",
             (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return server;
}
