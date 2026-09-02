#include "web_modules.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

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
    char resync_reason[32];

    ws_client_t clients[WEB_WS_MAX_CLIENTS];

    uint32_t resync_total;
    uint32_t send_error_total;
    uint32_t connect_total;
    uint32_t disconnect_total;

    portMUX_TYPE lock;
    httpd_handle_t server;
} web_event_ws_state_t;

static web_event_ws_state_t s_ws;
static bool s_initialized;

/* ── Forward declarations ───────────────────────────────────────────── */

static void web_event_ws_drain(void *arg);

/* ── Helpers ────────────────────────────────────────────────────────── */

static void lock_ws(void)
{
    portENTER_CRITICAL(&s_ws.lock);
}

static void unlock_ws(void)
{
    portEXIT_CRITICAL(&s_ws.lock);
}

static int count_clients_locked(void)
{
    int n = 0;
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (s_ws.clients[i].active) n++;
    }
    return n;
}

static bool validate_ws_fd(int fd)
{
    if (s_ws.server == NULL) {
        return false;
    }
    return httpd_ws_get_fd_info(s_ws.server, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

static void prune_stale_clients_locked(int stale_fds[WEB_WS_MAX_CLIENTS])
{
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (s_ws.clients[i].active &&
            !validate_ws_fd(s_ws.clients[i].fd)) {
            stale_fds[i] = s_ws.clients[i].fd;
            s_ws.clients[i].active = false;
        }
    }
}

static void log_pruned_stale_clients(
    const int stale_fds[WEB_WS_MAX_CLIENTS])
{
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (stale_fds[i] >= 0) {
            ESP_LOGI(TAG, "Pruned stale client fd=%d slot=%d",
                     stale_fds[i], i);
        }
    }
}

static bool register_client(int fd)
{
    int stale_fds[WEB_WS_MAX_CLIENTS];
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        stale_fds[i] = -1;
    }

    lock_ws();

    /* Check if this exact fd is already registered */
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (s_ws.clients[i].active && s_ws.clients[i].fd == fd) {
            unlock_ws();
            ESP_LOGD(TAG, "Client fd=%d already registered slot=%d", fd, i);
            return true;
        }
    }

    /* Prune stale slots before enforcing limit */
    prune_stale_clients_locked(stale_fds);

    if (count_clients_locked() >= WEB_WS_MAX_CLIENTS) {
        unlock_ws();
        log_pruned_stale_clients(stale_fds);
        ESP_LOGW(TAG, "Client rejected: limit %d reached", WEB_WS_MAX_CLIENTS);
        return false;
    }

    /* Handle FD reuse: clear any inactive slot with same numeric fd */
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (!s_ws.clients[i].active) {
            s_ws.clients[i].fd = fd;
            s_ws.clients[i].active = true;
            s_ws.connect_total++;
            unlock_ws();
            log_pruned_stale_clients(stale_fds);
            ESP_LOGI(TAG, "Client registered fd=%d slot=%d", fd, i);
            return true;
        }
    }

    unlock_ws();
    log_pruned_stale_clients(stale_fds);
    return false;
}

static void prune_client(int fd)
{
    int pruned_slot = -1;

    lock_ws();
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (s_ws.clients[i].active && s_ws.clients[i].fd == fd) {
            s_ws.clients[i].active = false;
            s_ws.disconnect_total++;
            pruned_slot = i;
            break;
        }
    }
    unlock_ws();

    if (pruned_slot >= 0) {
        ESP_LOGI(TAG, "Client pruned fd=%d slot=%d", fd, pruned_slot);
    }
}

/* ── JSON string escaping (bounded, no allocation) ─────────────────── */

static int json_escape_string(const char *src, char *dst, size_t dst_len)
{
    if (src == NULL || dst == NULL || dst_len < 2) {
        return -1;
    }

    size_t di = 0;
    dst[di++] = '"';

    for (const char *p = src; *p != '\0' && di < dst_len - 2; p++) {
        char c = *p;
        if (c == '"' || c == '\\') {
            if (di + 2 >= dst_len - 1) return -1;
            dst[di++] = '\\';
            dst[di++] = c;
        } else if ((uint8_t)c < 0x20) {
            if (di + 6 >= dst_len - 1) return -1;
            di += snprintf(dst + di, dst_len - di, "\\u%04x", (unsigned char)c);
        } else {
            dst[di++] = c;
        }
    }

    if (di >= dst_len - 1) return -1;
    dst[di++] = '"';
    dst[di] = '\0';
    return (int)di;
}

/* ── JSON serialization (bounded, no cJSON) ─────────────────────────── */

static int serialize_event(const gateway_event_t *ev, char *buf, size_t len)
{
    int n = 0;

    char esc_device[GW_MSG_DEVICE_ID_LEN * 4 + 3];
    char esc_feature[GW_FEATURE_ID_LEN * 4 + 3];
    if (json_escape_string(ev->device_id, esc_device, sizeof(esc_device)) < 0 ||
        json_escape_string(ev->feature_id, esc_feature, sizeof(esc_feature)) < 0) {
        return -1;
    }

    switch (ev->type) {
    case GW_EVENT_DEVICE_CONNECTION:
        n = snprintf(buf, len,
                     "{\"seq\":%" PRIu32 ",\"type\":\"device.connection\""
                     ",\"deviceId\":%s,\"connected\":%s}",
                     ev->seq, esc_device,
                     ev->bool_value ? "true" : "false");
        break;
    case GW_EVENT_DEVICE_CHANGED:
        n = snprintf(buf, len,
                     "{\"seq\":%" PRIu32 ",\"type\":\"device.changed\""
                     ",\"deviceId\":%s}",
                     ev->seq, esc_device);
        break;
    case GW_EVENT_DEVICE_SCHEMA:
        n = snprintf(buf, len,
                     "{\"seq\":%" PRIu32 ",\"type\":\"device.schema\""
                     ",\"deviceId\":%s,\"revision\":%" PRIu32 "}",
                     ev->seq, esc_device, ev->schema_revision);
        break;
    case GW_EVENT_FEATURE_STATE:
        if (ev->value_kind == GW_EVENT_VALUE_BOOL) {
            n = snprintf(buf, len,
                         "{\"seq\":%" PRIu32 ",\"type\":\"feature.state\""
                         ",\"deviceId\":%s,\"featureId\":%s"
                         ",\"propertyId\":%u,\"valueType\":\"bool\""
                         ",\"value\":%s,\"updatedAtMs\":%" PRId64 "}",
                         ev->seq, esc_device, esc_feature,
                         ev->property_id,
                         ev->bool_value ? "true" : "false",
                         ev->updated_at_ms);
        } else if (ev->value_kind == GW_EVENT_VALUE_INT) {
            n = snprintf(buf, len,
                         "{\"seq\":%" PRIu32 ",\"type\":\"feature.state\""
                         ",\"deviceId\":%s,\"featureId\":%s"
                         ",\"propertyId\":%u,\"valueType\":\"int\""
                         ",\"value\":%" PRId32
                         ",\"updatedAtMs\":%" PRId64 "}",
                         ev->seq, esc_device, esc_feature,
                         ev->property_id, ev->int_value,
                         ev->updated_at_ms);
        } else {
            n = snprintf(buf, len,
                         "{\"seq\":%" PRIu32 ",\"type\":\"feature.state\""
                         ",\"deviceId\":%s,\"featureId\":%s"
                         ",\"propertyId\":%u"
                         ",\"updatedAtMs\":%" PRId64 "}",
                         ev->seq, esc_device, esc_feature,
                         ev->property_id,
                         ev->updated_at_ms);
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
    int stale_fds[WEB_WS_MAX_CLIENTS];
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        stale_fds[i] = -1;
    }

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

    /* Snapshot client fds, validate they are still active WebSocket fds */
    int fds[WEB_WS_MAX_CLIENTS];
    int fd_count = 0;
    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (s_ws.clients[i].active) {
            if (validate_ws_fd(s_ws.clients[i].fd)) {
                fds[fd_count++] = s_ws.clients[i].fd;
            } else {
                stale_fds[i] = s_ws.clients[i].fd;
                s_ws.clients[i].active = false;
            }
        }
    }

    unlock_ws();

    for (int i = 0; i < WEB_WS_MAX_CLIENTS; i++) {
        if (stale_fds[i] >= 0) {
            ESP_LOGD(TAG, "Drain: pruned stale fd=%d slot=%d",
                     stale_fds[i], i);
        }
    }

    if (batch_count == 0 && !need_resync) {
        return;
    }

    /* Send resync.required first if flagged */
    if (need_resync) {
        char json[WEB_WS_JSON_MAX];
        int n = snprintf(json, sizeof(json),
                         "{\"seq\":%" PRIu32 ",\"type\":\"resync.required\""
                         ",\"reason\":\"%s\"}",
                         gateway_events_current_seq(),
                         s_ws.resync_reason);
        if (n > 0 && (size_t)n < sizeof(json)) {
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
                    lock_ws();
                    s_ws.send_error_total++;
                    unlock_ws();
                    prune_client(fds[i]);
                }
            }
            lock_ws();
            s_ws.resync_total++;
            unlock_ws();
        }
    }

    /* Broadcast each event */
    for (int e = 0; e < batch_count; e++) {
        char json[WEB_WS_JSON_MAX];
        int n = serialize_event(&batch[e], json, sizeof(json));
        if (n <= 0) {
            /* Serialize failure: flag resync for next drain cycle */
            lock_ws();
            s_ws.resync_required = true;
            strlcpy(s_ws.resync_reason, "serialize_failed",
                    sizeof(s_ws.resync_reason));
            s_ws.send_error_total++;
            unlock_ws();
            continue;
        }

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
                lock_ws();
                s_ws.send_error_total++;
                unlock_ws();
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
        strlcpy(s_ws.resync_reason, "ring_overflow",
                sizeof(s_ws.resync_reason));
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
            strlcpy(s_ws.resync_reason, "queue_work_failed",
                    sizeof(s_ws.resync_reason));
            unlock_ws();
        }
    }
}

/* ── WebSocket URI handler ────────────────────────────────────────────
 *
 * ESP-IDF 6.x WebSocket lifecycle:
 * - httpd_uri_t.is_websocket=true tells httpd to perform the WS handshake
 *   automatically before calling ws_post_handshake_cb.
 * - ws_post_handshake_cb is called after the handshake completes; this is
 *   the reliable registration point.
 * - handle_ws_control_frames=true enables PING/PONG/CLOSE frame handling
 *   at the httpd level; ws_control_handler receives CLOSE notifications.
 * - httpd_ws_send_frame_async() sends through the session socket directly
 *   in HTTPD context; it must NOT be called from producer tasks.
 *   The architecture routes through: producer -> ring -> httpd_queue_work()
 *   -> drain worker -> httpd_ws_send_frame_async().
 * - HTTPD purges the LRU socket when max_open_sockets is reached; our
 *   2-client limit prevents dashboard connections from being evicted.
 */

static esp_err_t web_event_ws_on_connect(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (!register_client(fd)) {
        ESP_LOGW(TAG, "Post-handshake registration failed fd=%d", fd);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t web_event_ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
    };
    uint8_t buf[WEB_WS_RX_MAX];
    frame.payload = buf;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    /* CLOSE frames: when handle_ws_control_frames=true, httpd handles
     * PING/PONG but passes CLOSE to the handler for cleanup. */
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        prune_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    /* Phase 0: ignore incoming text frames */
    return ESP_OK;
}

/* ── Registration ───────────────────────────────────────────────────── */

esp_err_t web_event_ws_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(&s_ws, 0, sizeof(s_ws));
    s_ws.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s_initialized = true;

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
        .ws_post_handshake_cb = web_event_ws_on_connect,
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

/* ── Stats for /api/status ──────────────────────────────────────────── */

void web_event_ws_get_stats(int *active_clients, uint32_t *ring_used,
                            bool *resync_pending, uint32_t *resync_total,
                            uint32_t *send_error_total,
                            uint32_t *connect_total,
                            uint32_t *disconnect_total)
{
    lock_ws();
    if (active_clients) *active_clients = count_clients_locked();
    if (ring_used) *ring_used = s_ws.count;
    if (resync_pending) *resync_pending = s_ws.resync_required;
    if (resync_total) *resync_total = s_ws.resync_total;
    if (send_error_total) *send_error_total = s_ws.send_error_total;
    if (connect_total) *connect_total = s_ws.connect_total;
    if (disconnect_total) *disconnect_total = s_ws.disconnect_total;
    unlock_ws();
}
