/* Captive HTTP funnel tests (spec v3 §17) against a real esp_http_server on
 * the loopback interface. Covers provisioning-mode behavior only: the
 * gateway-mode plain-404 guarantee is structural (the error handler is
 * registered exclusively by web_server_start_provisioning()). */

#include <stdio.h>
#include <string.h>

#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "unity.h"

#include "web_modules.h"
#include "web_http.h"

#define TEST_PORT     8391
#define RAW_BUF_LEN   2048

TEST_CASE("query values are URL-decoded before device lookup", "[web_server]")
{
    char value[32];
    TEST_ASSERT_EQUAL(
        ESP_OK,
        web_get_query_value(
            "device_id=AA%3ABB%3ACC%3ADD%3AEE%3AFF", "device_id", value,
            sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", value);

    TEST_ASSERT_EQUAL(
        ESP_OK,
        web_get_query_value("device_id=sensor%26room%3D1", "device_id", value,
                            sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("sensor&room=1", value);
}

TEST_CASE("query decoder rejects malformed and unsafe values", "[web_server]")
{
    char value[32];
    TEST_ASSERT_NOT_EQUAL(
        ESP_OK,
        web_get_query_value("device_id=bad%3", "device_id", value,
                            sizeof(value)));
    TEST_ASSERT_NOT_EQUAL(
        ESP_OK,
        web_get_query_value("device_id=bad%00id", "device_id", value,
                            sizeof(value)));
}

typedef struct {
    bool is_303;
    bool location_root;
    bool cache_no_store;
    bool transfer_chunked;
    bool content_gzip;
    size_t body_len;
} captive_response_t;

/* Case-insensitive line-start search, avoiding GNU-only strcasestr. */
static bool header_present(const char *raw, const char *header)
{
    size_t length = strlen(header);
    for (const char *cursor = raw; *cursor != '\0'; cursor++) {
        if ((cursor == raw || cursor[-1] == '\n') &&
            strncasecmp(cursor, header, length) == 0) {
            return true;
        }
    }
    return false;
}

static void parse_response(const char *raw, captive_response_t *out)
{
    memset(out, 0, sizeof(*out));
    out->is_303 = strncmp(raw, "HTTP/1.1 303", 12) == 0;
    out->location_root = header_present(raw, "Location: /\r\n");
    out->cache_no_store = header_present(raw, "Cache-Control: no-store\r\n");
    out->transfer_chunked =
        header_present(raw, "Transfer-Encoding: chunked\r\n");
    out->content_gzip = header_present(raw, "Content-Encoding: gzip\r\n");

    const char *body = strstr(raw, "\r\n\r\n");
    out->body_len = body != NULL ? strlen(body + 4) : 0;
}

static bool fetch(const char *path, captive_response_t *out, char *raw,
                  size_t raw_capacity)
{
    memset(out, 0, sizeof(*out));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(TEST_PORT),
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
    };
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return false;
    }

    char request[128];
    int written = snprintf(request, sizeof(request),
                           "GET %s HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Connection: close\r\n\r\n",
                           path);
    if (written <= 0 || send(fd, request, (size_t)written, 0) != written) {
        close(fd);
        return false;
    }

    size_t stored = 0;
    size_t received_total = 0;
    char discard[256];
    for (;;) {
        size_t available = raw_capacity - 1 - stored;
        char *destination = available > 0 ? raw + stored : discard;
        size_t receive_size = available > 0 ? available : sizeof(discard);
        ssize_t received = recv(fd, destination, receive_size, 0);
        if (received <= 0) break;
        received_total += (size_t)received;
        if (available > 0) stored += (size_t)received;
    }
    close(fd);
    raw[stored] = '\0';

    parse_response(raw, out);
    return received_total > 0;
}

static httpd_handle_t start_test_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = TEST_PORT;
    config.ctrl_port = TEST_PORT + 1;

    httpd_handle_t server = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, httpd_start(&server, &config));
    TEST_ASSERT_NOT_NULL(server);
    TEST_ASSERT_EQUAL(ESP_OK, web_assets_register_provisioning(server));
    TEST_ASSERT_EQUAL(ESP_OK,
                      web_assets_register_provisioning_errors(server));
    return server;
}

TEST_CASE("known probes redirect with full captive response", "[web_server]")
{
    static const char *probes[] = {
        "/generate_204", "/hotspot-detect.html", "/connecttest.txt",
        "/ncsi.txt",
    };

    httpd_handle_t server = start_test_server();
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        captive_response_t response;
        char raw[RAW_BUF_LEN];
        TEST_ASSERT_TRUE_MESSAGE(fetch(probes[i], &response, raw,
                                       sizeof(raw)), probes[i]);
        TEST_ASSERT_TRUE_MESSAGE(response.is_303, probes[i]);
        TEST_ASSERT_TRUE_MESSAGE(response.location_root, probes[i]);
        TEST_ASSERT_TRUE_MESSAGE(response.cache_no_store, probes[i]);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, (int)response.body_len,
                                             probes[i]);
    }
    TEST_ASSERT_EQUAL(ESP_OK, httpd_stop(server));
    /* Let lingering TIME_WAIT sessions drain before the next case rebinds. */
    vTaskDelay(pdMS_TO_TICKS(100));
}

TEST_CASE("unknown uri funnels to portal via 404 handler", "[web_server]")
{
    httpd_handle_t server = start_test_server();

    captive_response_t response;
    char raw[RAW_BUF_LEN];
    TEST_ASSERT_TRUE(fetch("/this-path-does-not-exist", &response, raw,
                           sizeof(raw)));
    TEST_ASSERT_TRUE(response.is_303);
    TEST_ASSERT_TRUE(response.location_root);
    TEST_ASSERT_TRUE(response.cache_no_store);
    TEST_ASSERT_GREATER_THAN_INT(0, (int)response.body_len);
    TEST_ASSERT_EQUAL(ESP_OK, httpd_stop(server));
    vTaskDelay(pdMS_TO_TICKS(100));
}

TEST_CASE("portal root serves setup page", "[web_server]")
{
    httpd_handle_t server = start_test_server();

    captive_response_t response;
    char raw[RAW_BUF_LEN];
    TEST_ASSERT_TRUE(fetch("/", &response, raw, sizeof(raw)));
    TEST_ASSERT_TRUE(strncmp(raw, "HTTP/1.1 200", 12) == 0);
    TEST_ASSERT_TRUE(response.transfer_chunked);
    TEST_ASSERT_TRUE(response.content_gzip);
    TEST_ASSERT_GREATER_THAN_INT(0, (int)response.body_len);
    TEST_ASSERT_EQUAL(ESP_OK, httpd_stop(server));
    vTaskDelay(pdMS_TO_TICKS(100));
}

TEST_CASE("favicon stays 204 in provisioning mode", "[web_server]")
{
    httpd_handle_t server = start_test_server();

    captive_response_t response;
    char raw[512];
    TEST_ASSERT_TRUE(fetch("/favicon.ico", &response, raw, sizeof(raw)));
    TEST_ASSERT_TRUE(strncmp(raw, "HTTP/1.1 204", 12) == 0);
    TEST_ASSERT_EQUAL(ESP_OK, httpd_stop(server));
    vTaskDelay(pdMS_TO_TICKS(100));
}
