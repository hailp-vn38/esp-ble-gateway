#include <stdio.h>
#include <string.h>

#include "lwip/sockets.h"

#include "command_dispatcher.h"
#include "device_management_internal.h"
#include "device_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_events.h"
#include "unity.h"
#include "web_modules.h"

#include "../../command_dispatcher/command_dispatcher_internal.h"

#define BASELINE_PORT 8393
#define RESPONSE_LEN  4096

static bool s_forget_peer_ok;

static ble_central_err_t management_status(
    const char *device_id, ble_central_device_status_t *status)
{
    (void)device_id;
    memset(status, 0, sizeof(*status));
    return BLE_CENTRAL_OK;
}

static esp_err_t management_schema_get(const char *device_id,
                                       device_schema_snapshot_t *snapshot)
{
    (void)device_id;
    memset(snapshot, 0, sizeof(*snapshot));
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t management_ok(const char *device_id)
{
    (void)device_id;
    return ESP_OK;
}

static int management_connect(const char *device_id, const uint8_t *address,
                              uint8_t address_type)
{
    (void)device_id;
    (void)address;
    (void)address_type;
    return BLE_CENTRAL_OK;
}

static int management_forget_peer(const char *device_id,
                                  const uint8_t *address,
                                  uint8_t address_type, bool has_identity)
{
    (void)device_id;
    (void)address;
    (void)address_type;
    (void)has_identity;
    return s_forget_peer_ok ? BLE_CENTRAL_OK : BLE_CENTRAL_ERR_STACK;
}

static void management_publish(gateway_event_t *event)
{
    (void)event;
}

static void install_management_hooks(void)
{
    device_management_hooks_t hooks = {
        .connect = management_connect,
        .get_status = management_status,
        .forget_peer = management_forget_peer,
        .schema_get = management_schema_get,
        .schema_forget = management_ok,
        .cancel_commands = management_ok,
        .publish = management_publish,
    };
    s_forget_peer_ok = true;
    device_management_set_hooks(&hooks);
}

static bool request_api(const char *method, const char *path, const char *body,
                        char response[RESPONSE_LEN])
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(BASELINE_PORT),
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
    };
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return false;
    }

    size_t body_len = body != NULL ? strlen(body) : 0;
    char request[1024];
    int length = snprintf(request, sizeof(request),
                          "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %u\r\nConnection: close\r\n\r\n%s",
                          method, path, (unsigned)body_len,
                          body != NULL ? body : "");
    if (length <= 0 || length >= (int)sizeof(request) ||
        send(fd, request, (size_t)length, 0) != length) {
        close(fd);
        return false;
    }
    size_t stored = 0;
    while (stored < RESPONSE_LEN - 1) {
        ssize_t received = recv(fd, response + stored,
                                RESPONSE_LEN - 1 - stored, 0);
        if (received <= 0) break;
        stored += (size_t)received;
    }
    close(fd);
    response[stored] = '\0';
    return stored > 0;
}

static httpd_handle_t start_api_server(void)
{
    gateway_events_reset_for_test();
    install_management_hooks();
    command_dispatcher_reset_for_test();
    TEST_ASSERT_EQUAL(0, command_dispatcher_init());
    TEST_ASSERT_EQUAL(0, command_dispatcher_freeze_registry());

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = BASELINE_PORT;
    config.ctrl_port = BASELINE_PORT + 1;
    httpd_handle_t server = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, httpd_start(&server, &config));
    TEST_ASSERT_EQUAL(ESP_OK, web_device_api_register(server));
    TEST_ASSERT_EQUAL(ESP_OK, web_command_api_register(server));
    return server;
}

static void stop_api_server(httpd_handle_t server)
{
    TEST_ASSERT_EQUAL(ESP_OK, httpd_stop(server));
    vTaskDelay(pdMS_TO_TICKS(100));
    device_management_set_hooks(NULL);
}

TEST_CASE("Web device CRUD baseline preserves HTTP contract",
          "[web_server][baseline]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];

    TEST_ASSERT_TRUE(request_api("GET", "/api/devices", NULL, response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200"));

    TEST_ASSERT_TRUE(request_api("POST", "/api/devices", "{}", response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 400"));
    TEST_ASSERT_NOT_NULL(strstr(response, "invalid_request"));

    TEST_ASSERT_TRUE(request_api("PUT", "/api/devices", "{}", response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 400"));
    TEST_ASSERT_NOT_NULL(strstr(response, "invalid_request"));

    TEST_ASSERT_TRUE(request_api("DELETE", "/api/devices", NULL, response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 400"));
    TEST_ASSERT_NOT_NULL(strstr(response, "invalid_request"));

    stop_api_server(server);
}

TEST_CASE("Web command baseline rejects missing typed command fields",
          "[web_server][baseline]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];
    TEST_ASSERT_TRUE(request_api("POST", "/api/command", "{}", response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 400"));
    TEST_ASSERT_NOT_NULL(strstr(response, "invalid_request"));
    stop_api_server(server);
}

TEST_CASE("Web device typed CRUD preserves inventory contract",
          "[web_server][device_management]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];

    TEST_ASSERT_TRUE(request_api("GET", "/api/devices", NULL, response));
    TEST_ASSERT_NOT_NULL(strstr(response, "\r\n\r\n[]"));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-Gateway-Event-Seq:"));

    TEST_ASSERT_TRUE(request_api(
        "POST", "/api/devices",
        "{\"device_id\":\"web-a\",\"name\":\"Kitchen\","
        "\"ble_addr\":\"11:22:33:44:55:66\",\"ble_addr_type\":1}",
        response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"persisted\":true"));

    TEST_ASSERT_TRUE(request_api(
        "POST", "/api/devices",
        "{\"device_id\":\"web-b\",\"name\":\"Bedroom\"}", response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200"));

    TEST_ASSERT_TRUE(request_api("GET", "/api/devices", NULL, response));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"device_id\":\"web-a\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"name\":\"Kitchen\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"capabilities\":"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"controls\":[]"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"ble_addr\":\"11:22:33:44:55:66\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"device_id\":\"web-b\""));

    TEST_ASSERT_TRUE(request_api(
        "POST", "/api/devices",
        "{\"device_id\":\"web-a\",\"name\":\"Duplicate\"}", response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 409"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"conflict\""));

    TEST_ASSERT_TRUE(request_api(
        "PUT", "/api/devices",
        "{\"device_id\":\"web-a\",\"name\":\"Renamed\"}", response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200"));
    TEST_ASSERT_NOT_NULL(strstr(response, "Device updated"));

    TEST_ASSERT_TRUE(request_api(
        "PUT", "/api/devices",
        "{\"device_id\":\"web-missing\",\"name\":\"Missing\"}", response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 404"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"device_not_found\""));

    TEST_ASSERT_TRUE(request_api(
        "DELETE", "/api/devices?device_id=web-a", NULL, response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"store_deleted\":true"));

    TEST_ASSERT_TRUE(request_api(
        "DELETE", "/api/devices?device_id=web-a", NULL, response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 404"));

    stop_api_server(server);
}

TEST_CASE("Web device delete exposes degraded typed cleanup",
          "[web_server][device_management]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];
    TEST_ASSERT_EQUAL(DEVICE_STORE_OK,
                      device_store_add("web-degraded", "Degraded"));
    s_forget_peer_ok = false;

    TEST_ASSERT_TRUE(request_api(
        "DELETE", "/api/devices?device_id=web-degraded", NULL, response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 207"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"cleanup_degraded\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"ble_peer_forgotten\":false"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"store_deleted\":true"));

    stop_api_server(server);
}
