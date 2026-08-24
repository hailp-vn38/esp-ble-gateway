#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "web_server.h"
#include "cbor_codec.h"
#include "command_dispatcher.h"
#include "device_store.h"
#include "log_buffer.h"
#include "wifi_prov.h"

static const char *TAG = "web_server";

static const char *INDEX_HTML =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<title>ESP32 BLE Gateway</title>"
"<style>body{font-family:sans-serif;margin:2em;}table{border-collapse:collapse;width:100%}"
"td,th{border:1px solid #ccc;padding:6px;text-align:left}"
"#log{background:#111;color:#0f0;padding:1em;height:200px;overflow-y:scroll;font-family:monospace;font-size:12px}"
"</style></head><body>"
"<h2>ESP32 BLE Gateway - Quan ly thiet bi</h2>"
"<div><input id='did' placeholder='device_id'><input id='dname' placeholder='name'>"
"<button onclick='addDevice()'>Add</button></div>"
"<table id='devTable'><tr><th>ID</th><th>Ten</th><th>Loai</th><th>Ket noi</th><th></th></tr></table>"
"<h3>Log</h3><div id='log'></div>"
"<script>"
"async function refresh(){"
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
    snprintf(json, sizeof(json), "{\"device_count\":%d,\"ip\":\"%s\",\"wifi_connected\":%s}",
             count, ip, wifi_prov_is_connected() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
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

    char ssid[33] = {0};
    char pass[65] = {0};
    char *p = strstr(buf, "\"ssid\":\"");
    if (p) sscanf(p + 8, "%32[^\"]", ssid);
    p = strstr(buf, "\"password\":\"");
    if (p) sscanf(p + 12, "%64[^\"]", pass);

    int rc = wifi_prov_save_and_connect(ssid, pass);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rc == 0 ? "{\"success\":true}" : "{\"success\":false}", HTTPD_RESP_USE_STRLEN);
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
        { .uri = "/api/wifi",     .method = HTTP_POST,   .handler = wifi_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "Web server started with %d routes", (int)(sizeof(routes) / sizeof(routes[0])));
    return server;
}
