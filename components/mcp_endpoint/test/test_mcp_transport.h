// Shared transport-hook mock for mcp_endpoint unit tests.
//
// Include from exactly one Unity translation unit per fixture set; each
// including file gets its own private mock state, which is fine because
// Unity runs test cases sequentially.
//
// The mock feeds a canned body into the endpoint and captures the JSON-RPC
// response, HTTP status line and interesting headers. Header lookups are
// served from a small name/value table.

#ifndef MCP_TEST_TRANSPORT_H
#define MCP_TEST_TRANSPORT_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "unity.h"

#include "../mcp_endpoint_internal.h"

#define MCP_MOCK_MAX_HEADERS 8
#define MCP_MOCK_RESPONSE_CAP 4096

typedef struct {
    const char *name;
    const char *value;
} mcp_mock_header_t;

typedef struct {
    // request side
    const char *body;
    size_t body_len;
    size_t body_pos;
    int timeouts_before_data; // INT_MAX-style large value = always timeout
    int recv_calls;
    int recv_fail_after;      // -1 = never fail with a socket error

    mcp_mock_header_t headers[MCP_MOCK_MAX_HEADERS];
    int header_count;

    // response side
    char response[MCP_MOCK_RESPONSE_CAP];
    size_t response_len;
    int responses_sent;
    int errors_sent;
    char error_message[128];
    char status_line[48];
    bool connection_closed;
    char www_authenticate[32];

    int async_begins;
    int async_completes;
} mcp_mock_io_t;

static mcp_mock_io_t g_io;
static httpd_req_t g_req;

static void io_set_header(const char *name, const char *value)
{
    for (int i = 0; i < g_io.header_count; i++) {
        if (strcasecmp(g_io.headers[i].name, name) == 0) {
            g_io.headers[i].value = value;
            return;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(g_io.header_count < MCP_MOCK_MAX_HEADERS,
                             "mock header table full");
    g_io.headers[g_io.header_count].name = name;
    g_io.headers[g_io.header_count].value = value;
    g_io.header_count++;
}

static void io_reset(const char *body)
{
    memset(&g_io, 0, sizeof(g_io));
    g_io.body = body;
    g_io.body_len = body != NULL ? strlen(body) : 0;
    g_io.recv_fail_after = -1;
    // Default header set matching the test app's Kconfig: dev-mode token,
    // Host allowlist "gateway.local".
    io_set_header("Content-Type", "application/json");
    io_set_header("Host", "gateway.local");
}

static cJSON *io_response_json(void)
{
    TEST_ASSERT_EQUAL_INT(1, g_io.responses_sent);
    return cJSON_Parse(g_io.response);
}

static int mock_recv(httpd_req_t *req, char *buf, int buf_len)
{
    (void)req;
    g_io.recv_calls++;
    if (g_io.recv_calls > g_io.recv_fail_after && g_io.recv_fail_after >= 0) {
        return -1; // generic socket failure
    }
    if (g_io.timeouts_before_data > 0) {
        g_io.timeouts_before_data--;
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    if (g_io.body_pos >= g_io.body_len) return 0;
    size_t remaining = g_io.body_len - g_io.body_pos;
    size_t n = (size_t)buf_len < remaining ? (size_t)buf_len : remaining;
    memcpy(buf, g_io.body + g_io.body_pos, n);
    g_io.body_pos += n;
    return (int)n;
}

static esp_err_t mock_send(httpd_req_t *req, const char *buf, size_t len)
{
    (void)req;
    // NOTE: responding after Connection: close is expected — every
    // early-reject path sets that header immediately before its one reply.
    g_io.responses_sent++;
    size_t real_len = len;
    if ((ssize_t)len == HTTPD_RESP_USE_STRLEN || buf == NULL) {
        real_len = buf != NULL ? strlen(buf) : 0;
    }
    if (real_len > sizeof(g_io.response) - 1) {
        real_len = sizeof(g_io.response) - 1;
    }
    if (real_len > 0) memcpy(g_io.response, buf, real_len);
    g_io.response_len = real_len;
    g_io.response[real_len] = '\0';
    return ESP_OK;
}

static esp_err_t mock_send_err(httpd_req_t *req, int status,
                               const char *message)
{
    (void)req;
    g_io.errors_sent++;
    g_io.responses_sent++;
    snprintf(g_io.status_line, sizeof(g_io.status_line), "%d", status);
    snprintf(g_io.error_message, sizeof(g_io.error_message), "%s",
             message != NULL ? message : "");
    return ESP_OK;
}

static esp_err_t mock_set_type(httpd_req_t *req, const char *type)
{
    (void)req;
    (void)type;
    return ESP_OK;
}

static esp_err_t mock_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    snprintf(g_io.status_line, sizeof(g_io.status_line), "%s", status);
    return ESP_OK;
}

static esp_err_t mock_set_hdr(httpd_req_t *req, const char *field,
                              const char *value)
{
    (void)req;
    if (strcasecmp(field, "Connection") == 0) {
        g_io.connection_closed = strcasecmp(value, "close") == 0;
    } else if (strcasecmp(field, "WWW-Authenticate") == 0) {
        snprintf(g_io.www_authenticate, sizeof(g_io.www_authenticate), "%s",
                 value);
    }
    return ESP_OK;
}

static char *mock_get_header(httpd_req_t *req, const char *name)
{
    (void)req;
    for (int i = 0; i < g_io.header_count; i++) {
        if (strcasecmp(g_io.headers[i].name, name) == 0 &&
            g_io.headers[i].value != NULL) {
            char *copy = malloc(strlen(g_io.headers[i].value) + 1);
            if (copy != NULL) strcpy(copy, g_io.headers[i].value);
            return copy;
        }
    }
    return NULL;
}

static httpd_req_t g_async_req_holder;
static bool g_async_req_seeded;

static esp_err_t mock_async_begin(httpd_req_t *req, httpd_req_t **out)
{
    (void)req;
    g_io.async_begins++;
    if (!g_async_req_seeded) {
        memset(&g_async_req_holder, 0, sizeof(g_async_req_holder));
        g_async_req_seeded = true;
    }
    *out = &g_async_req_holder;
    return ESP_OK;
}

static esp_err_t mock_async_complete(httpd_req_t *req)
{
    (void)req;
    g_io.async_completes++;
    return ESP_OK;
}

static void install_transport(void)
{
    static const mcp_transport_t hooks = {
        .recv = mock_recv,
        .send = mock_send,
        .send_err = mock_send_err,
        .set_type = mock_set_type,
        .set_status = mock_set_status,
        .set_hdr = mock_set_hdr,
        .get_header = mock_get_header,
        .async_begin = mock_async_begin,
        .async_complete = mock_async_complete,
    };
    mcp_transport_set(&hooks);
}

// Runs one full request cycle against the mocked transport.
static esp_err_t run_request(const char *body)
{
    io_reset(body);
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;
    return mcp_handle_request(&g_req);
}

#endif /* MCP_TEST_TRANSPORT_H */
