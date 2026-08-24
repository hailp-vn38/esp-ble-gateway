#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "web_server.h"
#include "cbor_codec.h"
#include "command_dispatcher.h"
#include "device_store.h"
#include "log_buffer.h"
#include "wifi_prov.h"

static const char *TAG = "web_server";

static bool json_append(char *buf, size_t cap, size_t *offset, const char *format, ...)
{
    if (*offset >= cap) return false;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buf + *offset, cap - *offset, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= cap - *offset) return false;
    *offset += (size_t)written;
    return true;
}

static bool json_append_escaped(char *buf, size_t cap, size_t *offset, const char *text)
{
    if (!json_append(buf, cap, offset, "\"")) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            if (!json_append(buf, cap, offset, "\\%c", *p)) return false;
        } else if (*p < 0x20) {
            if (!json_append(buf, cap, offset, "\\u%04x", *p)) return false;
        } else if (!json_append(buf, cap, offset, "%c", *p)) {
            return false;
        }
    }
    return json_append(buf, cap, offset, "\"");
}

static int json_extract_string(const char *json, const char *key, char *out, size_t out_cap)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return -1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != '"') return -1;

    size_t len = 0;
    while (*p != '\0' && *p != '"') {
        unsigned char value = (unsigned char)*p++;
        if (value == '\\') {
            char escaped = *p++;
            if (escaped == '\0') return -1;
            switch (escaped) {
                case '"': value = '"'; break;
                case '\\': value = '\\'; break;
                case '/': value = '/'; break;
                case 'b': value = '\b'; break;
                case 'f': value = '\f'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                default: return -1;
            }
        }
        if (len + 1 >= out_cap) return -1;
        out[len++] = (char)value;
    }
    if (*p != '"') return -1;
    out[len] = '\0';
    return 0;
}

static const char *INDEX_HTML =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 BLE Gateway</title>"
"<style>body{font-family:sans-serif;margin:2em;max-width:900px}table{border-collapse:collapse;width:100%}"
"td,th{border:1px solid #ccc;padding:6px;text-align:left}"
".card{border:1px solid #ccc;border-radius:8px;padding:18px;margin-bottom:20px}"
"input,button{padding:9px;margin:4px}input{min-width:220px}button{cursor:pointer}"
"#wifiMsg{white-space:pre-wrap;margin-top:10px}.ok{color:#087b32}.err{color:#b42318}"
"#log{background:#111;color:#0f0;padding:1em;height:200px;overflow-y:scroll;font-family:monospace;font-size:12px}"
"</style></head><body>"
"<h2>ESP32 BLE Gateway</h2>"
"<div id='wifiCard' class='card' style='display:none'>"
"<h3>Cau hinh Wi-Fi ban dau</h3>"
"<p>Chon mang Wi-Fi, nhap mat khau. Gateway se thu ket noi truoc khi luu va khoi dong lai.</p>"
"<div><input id='wifiSsid' list='wifiList' placeholder='Ten Wi-Fi (SSID)'>"
"<datalist id='wifiList'></datalist><button id='scanBtn' onclick='scanWifi()'>Quet Wi-Fi</button></div>"
"<div><input id='wifiPass' type='password' placeholder='Mat khau Wi-Fi'>"
"<button id='connectBtn' onclick='configureWifi()'>Kiem tra va ket noi</button></div>"
"<div id='wifiMsg'></div></div>"
"<div id='gatewayPanel' style='display:none'>"
"<h3>Quan ly thiet bi</h3>"
"<div><input id='did' placeholder='device_id'><input id='dname' placeholder='name'>"
"<button onclick='addDevice()'>Add</button></div>"
"<table id='devTable'><tr><th>ID</th><th>Ten</th><th>Loai</th><th>Ket noi</th><th></th></tr></table>"
"<h3>Log</h3><div id='log'></div></div>"
"<script>"
"let wifiScanned=false,operationBusy=false,statusLoaded=false;"
"function wifiMessage(text,ok){let e=document.getElementById('wifiMsg');e.textContent=text;e.className=ok?'ok':'err';}"
"async function refreshStatus(){if(operationBusy)return;try{let r=await fetch('/api/status');if(!r.ok)throw new Error('HTTP '+r.status);let s=await r.json();statusLoaded=true;"
"  document.getElementById('wifiCard').style.display=s.provisioning?'block':'none';"
"  document.getElementById('gatewayPanel').style.display=s.provisioning?'none':'block';"
"  if(s.provisioning&&!wifiScanned){wifiScanned=true;scanWifi();}}catch(e){if(!statusLoaded)wifiMessage('Khong doc duoc trang thai gateway. Hay tai lai trang.',false);else console.warn(e);}}"
"async function scanWifi(){if(operationBusy)return;operationBusy=true;wifiScanned=true;let b=document.getElementById('scanBtn');b.disabled=true;wifiMessage('Dang quet Wi-Fi...',true);"
" try{let r=await fetch('/api/wifi/scan');let data=await r.json();if(!data.success)throw new Error(data.message||'Quet that bai');"
" let list=document.getElementById('wifiList');list.replaceChildren();data.networks.forEach(n=>{let o=document.createElement('option');"
" o.value=n.ssid;o.label=n.ssid+' ('+n.rssi+' dBm'+(n.secure?', bao mat':', mo')+')';list.appendChild(o);});"
" wifiScanned=true;wifiMessage(data.networks.length?'Da tim thay '+data.networks.length+' mang Wi-Fi':'Khong tim thay mang Wi-Fi. Ban co the nhap SSID thu cong.',true);"
" }catch(e){wifiMessage(e.message,false);}finally{operationBusy=false;b.disabled=false;}}"
"async function configureWifi(){let ssid=document.getElementById('wifiSsid').value.trim();let pass=document.getElementById('wifiPass').value;"
" if(!ssid){wifiMessage('Vui long chon hoac nhap SSID.',false);return;}if(operationBusy)return;operationBusy=true;let b=document.getElementById('connectBtn');b.disabled=true;"
" wifiMessage('Dang thu ket noi toi '+ssid+'. Vui long cho...',true);try{let r=await fetch('/api/wifi',{method:'POST',"
" headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,password:pass})});let data=await r.json();"
" if(!data.success)throw new Error(data.message||'Khong the ket noi');wifiMessage('Ket noi thanh cong. Da luu cau hinh, gateway dang khoi dong lai...',true);"
" }catch(e){wifiMessage(e.message,false);operationBusy=false;b.disabled=false;}}"
"async function refresh(){"
"  await refreshStatus();if(document.getElementById('gatewayPanel').style.display==='none')return;"
"  let r=await fetch('/api/devices'); let devices=await r.json();"
"  let t=document.getElementById('devTable');"
"  t.innerHTML='<tr><th>ID</th><th>Ten</th><th>Loai</th><th>Ket noi</th><th></th></tr>';"
"  devices.forEach(d=>{t.innerHTML+=`<tr><td>${d.device_id}</td><td>${d.name}</td>"
"    <td>${d.type}</td><td>${d.connected?'Yes':'No'}</td>"
"    <td><button onclick=\"delDevice('${d.device_id}')\">Xoa</button></td></tr>`});"
"  let lr=await fetch('/api/logs'); let logs=await lr.json();"
"  document.getElementById('log').innerHTML=logs.map(l=>l.text).join('<br>');"
"}"
"async function addDevice(){"
"  let id=document.getElementById('did').value; let name=document.getElementById('dname').value;"
"  await fetch('/api/devices',{method:'POST',headers:{'Content-Type':'application/json'},"
"    body:JSON.stringify({device_id:id,name:name,type:'generic'})}); refresh();"
"}"
"async function delDevice(id){"
"  await fetch('/api/devices?device_id='+id,{method:'DELETE'}); refresh();"
"}"
"setInterval(refresh,3000); refresh();"
"</script></body></html>";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t devices_get_handler(httpd_req_t *req)
{
    int count = 0;
    const device_entry_t *list = device_store_list(&count);

    char json[1024];
    int offset = snprintf(json, sizeof(json), "[");
    for (int i = 0; i < count && offset < (int)sizeof(json) - 1; i++) {
        offset += snprintf(json + offset, sizeof(json) - offset,
            "%s{\"device_id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"connected\":%s}",
            (i > 0) ? "," : "", list[i].device_id, list[i].name, list[i].type,
            list[i].connected ? "true" : "false");
    }
    snprintf(json + offset, sizeof(json) - offset, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t devices_post_handler(httpd_req_t *req)
{
    char buf[256];
    int recv_len = req->content_len;
    if (recv_len >= (int)sizeof(buf)) recv_len = sizeof(buf) - 1;
    int len = httpd_req_recv(req, buf, recv_len);
    if (len <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[len] = '\0';

    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};
    char name[DEVICE_NAME_MAX_LEN] = {0};
    const char *type = "generic";

    char *p = strstr(buf, "\"device_id\":\"");
    if (p) sscanf(p + 13, "%31[^\"]", device_id);
    p = strstr(buf, "\"name\":\"");
    if (p) sscanf(p + 8, "%31[^\"]", name);

    if (strlen(device_id) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing device_id");
        return ESP_FAIL;
    }

    int rc = device_store_add(device_id, strlen(name) > 0 ? name : device_id, type);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rc == 0 ? "{\"success\":true}" : "{\"success\":false}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t devices_delete_handler(httpd_req_t *req)
{
    char query[128];
    char device_id[GW_MSG_DEVICE_ID_LEN] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "device_id", device_id, sizeof(device_id));
    }

    if (strlen(device_id) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing device_id");
        return ESP_FAIL;
    }

    int rc = device_store_delete(device_id);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rc == 0 ? "{\"success\":true}" : "{\"success\":false}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    log_entry_t entries[LOG_BUFFER_CAPACITY];
    int count = log_buffer_get_all(entries);

    char json[2048];
    int offset = snprintf(json, sizeof(json), "[");
    for (int i = 0; i < count && offset < (int)sizeof(json) - 1; i++) {
        offset += snprintf(json + offset, sizeof(json) - offset,
            "%s{\"text\":\"%s\",\"timestamp_ms\":%ld}",
            (i > 0) ? "," : "", entries[i].text, entries[i].timestamp_ms);
    }
    snprintf(json + offset, sizeof(json) - offset, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    int count = 0;
    device_store_list(&count);

    char ip[16];
    wifi_prov_get_ip(ip, sizeof(ip));

    char json[256];
    snprintf(json, sizeof(json),
             "{\"device_count\":%d,\"ip\":\"%s\",\"wifi_connected\":%s,\"provisioning\":%s}",
             count, ip, wifi_prov_is_connected() ? "true" : "false",
             wifi_prov_is_provisioning() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    wifi_prov_ap_record_t records[WIFI_PROV_MAX_SCAN_RESULTS];
    size_t count = 0;
    int rc = wifi_prov_scan(records, WIFI_PROV_MAX_SCAN_RESULTS, &count);

    char json[3072];
    size_t offset = 0;
    bool valid = json_append(json, sizeof(json), &offset,
                             "{\"success\":%s,\"networks\":[", rc == 0 ? "true" : "false");
    if (rc == 0) {
        for (size_t i = 0; i < count && valid; i++) {
            valid = json_append(json, sizeof(json), &offset, "%s{\"ssid\":", i == 0 ? "" : ",") &&
                    json_append_escaped(json, sizeof(json), &offset, records[i].ssid) &&
                    json_append(json, sizeof(json), &offset,
                                ",\"rssi\":%d,\"secure\":%s}", records[i].rssi,
                                records[i].authmode != 0 ? "true" : "false");
        }
    }
    valid = valid && json_append(json, sizeof(json), &offset, "]");
    if (rc != 0) {
        valid = valid && json_append(json, sizeof(json), &offset, ",\"message\":\"%s\"",
                                     rc == -2 ? "Gateway is not in provisioning mode" : "Wi-Fi scan failed");
    }
    valid = valid && json_append(json, sizeof(json), &offset, "}");
    if (!valid) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, (ssize_t)offset);
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char buf[256];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request size");
        return ESP_FAIL;
    }

    int total = 0;
    while (total < req->content_len) {
        int len = httpd_req_recv(req, buf + total, req->content_len - total);
        if (len == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (len <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        total += len;
    }
    buf[total] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (json_extract_string(buf, "ssid", ssid, sizeof(ssid)) != 0 || ssid[0] == '\0' ||
        json_extract_string(buf, "password", pass, sizeof(pass)) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID or password");
        return ESP_FAIL;
    }

    int rc = wifi_prov_test_and_save(ssid, pass);

    httpd_resp_set_type(req, "application/json");
    const char *response;
    if (rc == 0) {
        response = "{\"success\":true,\"message\":\"Wi-Fi verified and saved\",\"restarting\":true}";
    } else if (rc == -2) {
        response = "{\"success\":false,\"message\":\"Gateway is not in provisioning mode\"}";
    } else if (rc == -5) {
        response = "{\"success\":false,\"message\":\"Connection failed. Check the SSID and password, then try again.\"}";
    } else {
        response = "{\"success\":false,\"message\":\"Could not test or save this Wi-Fi configuration\"}";
    }
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    if (rc == 0) wifi_prov_schedule_restart(1500);
    return ESP_OK;
}

httpd_handle_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,    .handler = index_get_handler },
        { .uri = "/api/devices",  .method = HTTP_GET,    .handler = devices_get_handler },
        { .uri = "/api/devices",  .method = HTTP_POST,   .handler = devices_post_handler },
        { .uri = "/api/devices",  .method = HTTP_DELETE, .handler = devices_delete_handler },
        { .uri = "/api/logs",     .method = HTTP_GET,    .handler = logs_get_handler },
        { .uri = "/api/status",   .method = HTTP_GET,    .handler = status_get_handler },
        { .uri = "/api/wifi/scan", .method = HTTP_GET,  .handler = wifi_scan_get_handler },
        { .uri = "/api/wifi",     .method = HTTP_POST,   .handler = wifi_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "Web server started with %d routes", (int)(sizeof(routes) / sizeof(routes[0])));
    return server;
}
