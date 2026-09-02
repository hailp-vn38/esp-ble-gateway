#include "web_modules.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "gateway_events.h"

static const char *TAG = "web_event_ws";

/* ── Constants ──────────────────────────────────────────────────────── */

#define WEB_WS_MAX_CLIENTS       2
#define WEB_WS_EVENT_RING_DEPTH  32
#define WEB_WS_JSON_MAX          512
#define WEB_WS_RX_MAX            128

/* ── State ──────────────────────────────────────────────────────────── */

typedef struct {
    int fd;
    bool active;
} ws_client_t;

typedef struct {
    gateway_event_t events[WEB_WS_EVENT_RING_DEPTH];
    size_t read_index;
    size_t write_index;
    size_t count;

    bool work_pending;
    bool resync_required;

    ws_client_t clients[WEB_WS_MAX_CLIENTS];

    SemaphoreHandle_t mutex;
    httpd_handle_t server;
} web_event_ws_state_t;

static web_event_ws_state_t s_ws;

/* ── Forward declarations ───────────────────────────────────────────── */

static void web_event_ws_drain(void *arg);

/* ── Helpers ────────────────────────────────────────────────────────── */

static void lock_ws(void)
{
    xSemaphoreTake(s_ws.mutex, pdMS_TO_TICKS(1000));
}

static void unlock_ws(void)
{
    xSemaphoreGive(s_ws.mutex);
}

static int count_clients_locked(void)
{
    int n = 0;
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (s_ws.clients[i].active) n++;
    }
    return n;
}

static bool register_client(int fd)
{
    lock_ws();
    if (count_clients_locked() >= WEB_WS_MAX_CLIENTS) {
        unlock_ws();
        ESP_LOGW(TAG, "Client rejected: limit %d reached", WEB_WS_MAX_CLIENTS);
        return false;
    }
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (!s_ws.clients[i].active) {
            s_ws.clients[i].fd = fd;
            s_ws.clients[i].active = true;
            unlock_ws();
            ESP_LOGI(TAG, "Client registered fd=%d slot=%d", fd, i);
            return true;
        }
    }
    unlock_ws();
    return false;
}

static void prune_client(int fd)
{
    lock_ws();
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (s_ws.clients[i].active && s_ws.clients[i].fd == fd) {
            s_ws.clients[i].active = false;
            ESP_LOGI(TAG, "Client pruned fd=%d slot=%d", fd, i);
            break;
        }
    }
    unlock_ws();
}

/* ── JSON serialization (bounded, no cJSON) ─────────────────────────── */

static int serialize_event(const gateway_event_t *ev, char *buf, size_t len)
{
    int n = 0;

    switch (ev->type) {
    case GW_EVENT_DEVICE_CONNECTION:
        n = snprintf(buf, len,
                     "{\"seq\":%" PRIu32 ",\"type\":\"device.connection\""
                     ",\"deviceId\":\"%s\",\"connected\":%s}",
                     ev->seq, ev->device_id,
                     ev->bool_value ? "true" : "false");
        break;
    case GW_EVENT_DEVICE_CHANGED:
        n = snprintf(buf, len,
                     "{\"seq\":%" PRIu32 ",\"type\":\"device.changed\""
                     ",\"deviceId\":\"%s\"}",
                     ev->seq, ev->device_id);
        break;
    case GW_EVENT_DEVICE_SCHEMA:
        n = snprintf(buf, len,
                     "{\"seq\":%" PRIu32 ",\"type\":\"device.schema\""
                     ",\"deviceId\":\"%s\",\"revision\":%" PRIu32 "}",
                     ev->seq, ev->device_id, ev->schema_revision);
        break;
    case GW_EVENT_FEATURE_STATE:
        if (ev->value_kind == GW_EVENT_VALUE_BOOL) {
            n = snprintf(buf, len,
                         "{\"seq\":%" PRIu32 ",\"type\":\"feature.state\""
                         ",\"deviceId\":\"%s\",\"featureId\":\"%s\""
                         ",\"propertyId\":%u,\"valueType\":\"bool\""
                         ",\"value\":%s}",
                         ev->seq, ev->device_id, ev->feature_id,
                         ev->property_id,
                         ev->bool_value ? "true" : "false");
        } else if (ev->value_kind == GW_EVENT_VALUE_INT) {
            n = snprintf(buf, len,
                         "{\"seq\":%" PRIu32 ",\"type\":\"feature.state\""
                         ",\"deviceId\":\"%s\",\"featureId\":\"%s\""
                         ",\"propertyId\":%u,\"valueType\":\"int\""
                         ",\"value\":%" PRId32 "}",
                         ev->seq, ev->device_id, ev->feature_id,
                         ev->property_id, ev->int_value);
        } else {
            n = snprintf(buf, len,
                         "{\"seq\":%" PRIu32 ",\"type\":\"feature.state\""
                         ",\"deviceId\":\"%s\",\"featureId\":\"%s\""
                         ",\"propertyId\":%u}",
                         ev->seq, ev->device_id, ev->feature_id,
                         ev->property_id);
        }
        break;
    case GW_EVENT_RESYNC_REQUIRED:
        n = snprintf(buf, len,
                     "{\"seq\":%" PRIu32 ",\"type\":\"resync.required\"}",
                     ev->seq);
        break;
    default:
        return -1;
    }

    if (n < 0 || (size_t)n >= len) {
        s_ws.resync_required = true;
        return -1;
    }
    return n;
}

/* ── HTTPD worker: drain ring and broadcast ─────────────────────────── */

static void web_event_ws_drain(void *arg)
{
    (void)arg;
    gateway_event_t batch[WEB_WS_EVENT_RING_DEPTH];
    int batch_count = 0;
    bool need_resync = false;

    lock_ws();

    /* Copy events out of ring under lock */
    while (s_ws.count > 0 && batch_count < WEB_WS_EVENT_RING_DEPTH) {
        batch[batch_count++] = s_ws.events[s_ws.read_index];
        s_ws.read_index =
            (s_ws.read_index + 1) % WEB_WS_EVENT_RING_DEPTH;
        s_ws.count--;
    }

    need_resync = s_ws.resync_required;
    s_ws.resync_required = false;
    s_ws.work_pending = false;

    /* Snapshot client fds */
    int fds[WEB_WS_MAX_CLIENTS];
    int fd_count = 0;
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (s_ws.clients[i].active) {
            fds[fd_count++] = s_ws.clients[i].fd;
        }
    }

    unlock_ws();

    if (batch_count == 0 && !need_resync) {
        return;
    }

    /* Send resync.required first if flagged */
    if (need_resync) {
        gateway_event_t resync = {0};
        resync.type = GW_EVENT_RESYNC_REQUIRED;
        char json[WEB_WS_JSON_MAX];
        int n = serialize_event(&resync, json, sizeof(json));
        if (n > 0) {
            httpd_ws_frame_t frame = {
                .final = true,
                .fragmented = false,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)json,
                .len = (size_t)n,
            };
            for (int i = 0; i < fd_count; i++) {
                esp_err_t err = httpd_ws_send_frame_async(
                    s_ws.server, fds[i], &frame);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Resync send failed fd=%d: %s",
                             fds[i], esp_err_to_name(err));
                    prune_client(fds[i]);
                }
            }
        }
    }

    /* Broadcast each event */
    for (int e = 0; e < batch_count; e++) {
        char json[WEB_WS_JSON_MAX];
        int n = serialize_event(&batch[e], json, sizeof(json));
        if (n <= 0) continue;

        httpd_ws_frame_t frame = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len = (size_t)n,
        };

        for (int i = 0; i < fd_count; i++) {
            esp_err_t err = httpd_ws_send_frame_async(
                s_ws.server, fds[i], &frame);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Send failed fd=%d: %s",
                         fds[i], esp_err_to_name(err));
                prune_client(fds[i]);
            }
        }
    }
}

/* ── Gateway event listener (called from producer tasks) ────────────── */

static void on_gateway_event(const gateway_event_t *event, void *context)
{
    (void)context;
    bool schedule = false;

    lock_ws();

    if (s_ws.count == WEB_WS_EVENT_RING_DEPTH) {
        s_ws.resync_required = true;
    } else {
        s_ws.events[s_ws.write_index] = *event;
        s_ws.write_index =
            (s_ws.write_index + 1) % WEB_WS_EVENT_RING_DEPTH;
        s_ws.count++;
    }

    if (!s_ws.work_pending && s_ws.server != NULL) {
        s_ws.work_pending = true;
        schedule = true;
    }

    unlock_ws();

    if (schedule) {
        if (httpd_queue_work(s_ws.server, web_event_ws_drain, NULL) != ESP_OK) {
            lock_ws();
            s_ws.work_pending = false;
            s_ws.resync_required = true;
            unlock_ws();
        }
    }
}

/* ── WebSocket URI handler ──────────────────────────────────────────── */

static esp_err_t web_event_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Handshake: register client */
        int fd = httpd_req_to_sockfd(req);
        if (!register_client(fd)) {
            return ESP_FAIL;
        }

        /* Enable control frame handling (PING/PONG/CLOSE) */
        httpd_ws_frame_t pong = {
            .type = HTTPD_WS_TYPE_TEXT,
        };
        esp_err_t err = httpd_ws_send_frame_async(
            s_ws.server, fd, &pong);
        if (err != ESP_OK) {
            prune_client(fd);
        }
        return ESP_OK;
    }

    /* Handle incoming frames (server-push only in phase 0) */
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
    };
    uint8_t buf[WEB_WS_RX_MAX];
    frame.payload = buf;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    /* Phase 0: ignore incoming text frames */
    return ESP_OK;
}

/* ── Registration ───────────────────────────────────────────────────── */

esp_err_t web_event_ws_init(void)
{
    memset(&s_ws, 0, sizeof(s_ws));
    s_ws.mutex = xSemaphoreCreateMutex();
    if (s_ws.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t web_event_ws_register(httpd_handle_t server)
{
    if (s_ws.server != NULL) {
        return ESP_OK; /* already registered */
    }

    s_ws.server = server;

    /* Register gateway event listener */
    esp_err_t err = gateway_events_register(on_gateway_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Event listener registration failed: %s",
                 esp_err_to_name(err));
    }

    /* Register WebSocket route */
    static const httpd_uri_t ws_uri = {
        .uri = "/ws/events",
        .method = HTTP_GET,
        .handler = web_event_ws_handler,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };

    err = httpd_register_uri_handler(server, &ws_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /ws/events: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "/ws/events registered (max %d clients, ring %d)",
             WEB_WS_MAX_CLIENTS, WEB_WS_EVENT_RING_DEPTH);
    return ESP_OK;
}
