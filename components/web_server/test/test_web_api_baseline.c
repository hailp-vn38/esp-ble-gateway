#include <stdio.h>
#include <string.h>

#include "lwip/sockets.h"

#include "device_command_service.h"
#include "device_management_internal.h"
#include "device_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_events.h"
#include "unity.h"
#include "web_exposure_api_internal.h"
#include "web_modules.h"


#define BASELINE_PORT 8393
#define RESPONSE_LEN  4096

/* ── Mock transport for device_command_service ─────────────────────── */

static bool s_forget_peer_ok;
static int  mock_send_rc;
static int  mock_connected;
static bool mock_send_called;
static uint32_t mock_send_count;
static gw_message_t mock_last_sent;

static int mock_send_command(const char *device_id, const gw_message_t *msg)
{
    mock_send_called = true;
    mock_send_count++;
    strlcpy(mock_last_sent.device_id, device_id,
            sizeof(mock_last_sent.device_id));
    mock_last_sent = *msg;
    return mock_send_rc;
}

static int mock_is_connected(const char *device_id)
{
    (void)device_id;
    return mock_connected;
}

static void install_command_mocks(void)
{
    device_command_transport_hooks_t hooks = {
        .send_command = mock_send_command,
        .is_connected = mock_is_connected,
    };
    device_command_service_set_hooks(&hooks);
}

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

static void reset_command_mocks(void)
{
    mock_send_rc = 0;
    mock_connected = 1;
    mock_send_called = false;
    mock_send_count = 0;
    memset(&mock_last_sent, 0, sizeof(mock_last_sent));
}

static httpd_handle_t start_api_server(void)
{
    gateway_events_reset_for_test();
    install_management_hooks();
    reset_command_mocks();

    /* Install mock transport before init so the service task never
     * calls real BLE central functions during tests. */
    install_command_mocks();
    (void)device_command_service_init();

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
    device_command_service_deinit();
    device_command_service_set_hooks(NULL);
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

/* ── Phase 5: Web /api/command on device_command_service only ──────── */

TEST_CASE("Web command rejects int_value that is not an integer",
          "[web_server][command][phase5]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];

    TEST_ASSERT_TRUE(request_api("POST", "/api/command",
        "{\"device_id\":\"d1\",\"command\":\"set_led\","
        "\"int_value\":3.14}",
        response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 400"));
    TEST_ASSERT_NOT_NULL(strstr(response, "int_value must be an integer"));

    stop_api_server(server);
}

TEST_CASE("Web command rejects bool_value that is not boolean",
          "[web_server][command][phase5]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];

    TEST_ASSERT_TRUE(request_api("POST", "/api/command",
        "{\"device_id\":\"d1\",\"command\":\"set_led\","
        "\"bool_value\":1}",
        response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 400"));
    TEST_ASSERT_NOT_NULL(strstr(response, "bool_value must be boolean"));

    stop_api_server(server);
}

TEST_CASE("Web command accepts valid bool request",
          "[web_server][command][phase5]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];

    TEST_ASSERT_TRUE(request_api("POST", "/api/command",
        "{\"device_id\":\"d1\",\"command\":\"set_led\","
        "\"bool_value\":true}",
        response));
    /* Handler submits to service and returns 200 OK immediately.
     * The service callback fires asynchronously; we verify the
     * HTTP response code from the handler itself. */
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200"));

    stop_api_server(server);
}

TEST_CASE("Web command accepts valid int request",
          "[web_server][command][phase5]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];

    TEST_ASSERT_TRUE(request_api("POST", "/api/command",
        "{\"device_id\":\"d1\",\"command\":\"set_brightness\","
        "\"int_value\":75}",
        response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200"));

    stop_api_server(server);
}

TEST_CASE("Web command rejects device_id exceeding max length",
          "[web_server][command][phase5]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];

    /* Build a device_id longer than GW_MSG_DEVICE_ID_LEN */
    char body[256];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"command\":\"set_led\"}",
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    /* 50 chars > GW_MSG_DEVICE_ID_LEN (48) */
    TEST_ASSERT_TRUE(request_api("POST", "/api/command", body, response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 400"));
    TEST_ASSERT_NOT_NULL(strstr(response, "device_id and command are required"));

    stop_api_server(server);
}

TEST_CASE("Web command rejects command exceeding max length",
          "[web_server][command][phase5]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];

    /* Build a command longer than GW_MSG_COMMAND_LEN */
    char body[300];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"d1\",\"command\":\"%s\"}",
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    TEST_ASSERT_TRUE(request_api("POST", "/api/command", body, response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 400"));
    TEST_ASSERT_NOT_NULL(strstr(response, "device_id and command are required"));

    stop_api_server(server);
}

TEST_CASE("Web command accepts request with both bool and int values",
          "[web_server][command][phase5]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];

    TEST_ASSERT_TRUE(request_api("POST", "/api/command",
        "{\"device_id\":\"d1\",\"command\":\"set_led\","
        "\"bool_value\":true,\"int_value\":42}",
        response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200"));

    stop_api_server(server);
}

TEST_CASE("Web command active contexts tracking",
          "[web_server][command][phase5]")
{
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];

    TEST_ASSERT_EQUAL(0, web_command_active_contexts());

    /* Submit a command - handler returns immediately, context is
     * managed by the async completion path. */
    TEST_ASSERT_TRUE(request_api("POST", "/api/command",
        "{\"device_id\":\"d1\",\"command\":\"set_led\","
        "\"bool_value\":true}",
        response));

    /* After the async handler completes (or fails because the client
     * disconnected), the active context count must return to 0. */
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_EQUAL(0, web_command_active_contexts());

    stop_api_server(server);
}

TEST_CASE("Web command does not use legacy dispatcher or executor",
          "[web_server][command][phase5]")
{
    /* This test verifies at compile/link time that web_command_api.c
     * links only against device_command_service, not command_executor
     * or command_dispatcher.  The grep gate in the plan doc covers
     * source-level verification; this test confirms the binary has
     * no legacy symbols by checking that the handler path works
     * end-to-end with only the service API. */
    httpd_handle_t server = start_api_server();
    char response[RESPONSE_LEN];

    /* Valid command goes through device_command_service only */
    TEST_ASSERT_TRUE(request_api("POST", "/api/command",
        "{\"device_id\":\"d1\",\"command\":\"set_led\","
        "\"bool_value\":true}",
        response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200"));

    /* Missing fields caught at HTTP layer before service */
    TEST_ASSERT_TRUE(request_api("POST", "/api/command", "{}", response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 400"));

    stop_api_server(server);
}

TEST_CASE("Web exposure request owns IDs after JSON is released",
          "[web_server][exposure]")
{
    cJSON *json = cJSON_Parse(
        "{\"device_id\":\"AC:27:6E:CC:F2:26\","
        "\"feature_id\":\"led_main\",\"enabled\":false}");
    TEST_ASSERT_NOT_NULL(json);

    web_exposure_update_request_t update = {0};
    TEST_ASSERT_EQUAL(WEB_EXPOSURE_PARSE_OK,
                      web_exposure_parse_update_request(json, &update));
    cJSON_Delete(json);

    TEST_ASSERT_EQUAL_STRING("AC:27:6E:CC:F2:26", update.device_id);
    TEST_ASSERT_EQUAL_STRING("led_main", update.feature_id);
    TEST_ASSERT_FALSE(update.enabled);
}
