#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "unity.h"

#include "../command_dispatcher_internal.h"
#include "cbor_codec.h"
#include "command_dispatcher.h"
#include "command_executor.h"

#include "test_mcp_transport.h"

// Memory-stability and transport-robustness checks (plan step 8). True
// multi-client concurrency needs real hardware and is verified manually with
// parallel curl sessions; here we prove the single-task paths do not leak,
// fragment or spin.

static void mcp_setup(void)
{
    command_dispatcher_reset_for_test();
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_freeze_registry());
    mcp_auth_reset_rate_limit();
    // A previous suite may have left the executor running (aborted test);
    // device commands must take the synchronous fallback path by default.
    command_executor_deinit();
}

// Device hooks whose send never completes: the worker stays blocked inside
// its 2s ACK wait, keeping both queue slots occupied deterministically.
static int blocking_is_connected(const char *device_id)
{
    return device_id != NULL && device_id[0] != '\0' ? 1 : 0;
}

static int blocking_send(const char *device_id, const gw_message_t *msg)
{
    (void)device_id;
    (void)msg;
    return 0;
}

static void install_device_hooks_blocking(void)
{
    static const device_command_hooks_t hooks = {
        .send_command = blocking_send,
        .is_connected = blocking_is_connected,
    };
    device_command_set_hooks(&hooks);
}

#define STRESS_LEAK_TOLERANCE_BYTES (2 * 1024)

// Memory tests fire far more requests per second than the auth gate allows,
// so each iteration refreshes the token bucket. Gate behavior itself is
// covered by the rate-limit test in test_mcp_endpoint.c.
static esp_err_t run_stress_request(const char *body)
{
    mcp_auth_reset_rate_limit();
    return run_request(body);
}

TEST_CASE("tools/call loop is heap stable", "[mcp_stress]")
{
    mcp_setup();
    const char *request =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"get_status\"}}";

    // Warm up lazily allocated resources (dispatch mutex, cJSON caches).
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT(0, run_stress_request(request));
    }
    size_t before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);

    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_INT(0, run_stress_request(request));
        cJSON *response = io_response_json();
        TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(response, "result"));
        cJSON_Delete(response);
    }

    size_t delta = before - heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    TEST_ASSERT_LESS_THAN_UINT32(STRESS_LEAK_TOLERANCE_BYTES, delta);
}

TEST_CASE("tools/list loop is heap stable", "[mcp_stress]")
{
    mcp_setup();
    const char *request =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";

    for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_INT(0, run_stress_request(request));
    size_t before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);

    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL_INT(0, run_stress_request(request));
        cJSON *response = io_response_json();
        TEST_ASSERT_NOT_NULL(response);
        cJSON_Delete(response);
    }

    size_t delta = before - heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    TEST_ASSERT_LESS_THAN_UINT32(STRESS_LEAK_TOLERANCE_BYTES, delta);
}

TEST_CASE("error-path loop is heap stable", "[mcp_stress]")
{
    mcp_setup();
    // Mix of parse errors, invalid requests and unknown tools: every
    // rejection path must release everything it allocated.
    const char *requests[] = {
        "{\"jsonrpc\":",
        "[1,2,3]",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"nope\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"missing_tool\"}}",
    };

    for (int i = 0; i < 5; i++) {
        for (size_t r = 0; r < sizeof(requests) / sizeof(requests[0]); r++) {
            TEST_ASSERT_EQUAL_INT(0, run_stress_request(requests[r]));
            cJSON *response = io_response_json();
            TEST_ASSERT_NOT_NULL(
                cJSON_GetObjectItemCaseSensitive(response, "error"));
            cJSON_Delete(response);
        }
    }
    size_t before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);

    for (int i = 0; i < 50; i++) {
        for (size_t r = 0; r < sizeof(requests) / sizeof(requests[0]); r++) {
            TEST_ASSERT_EQUAL_INT(0, run_stress_request(requests[r]));
            cJSON *response = io_response_json();
            cJSON_Delete(response);
        }
    }
    size_t delta = before - heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    TEST_ASSERT_LESS_THAN_UINT32(STRESS_LEAK_TOLERANCE_BYTES, delta);
}

TEST_CASE("persistent recv timeout never spins forever", "[mcp_stress]")
{
    mcp_setup();
    io_reset("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
    g_io.timeouts_before_data = INT_MAX;
    install_transport();
    memset(&g_req, 0, sizeof(g_req));
    g_req.content_len = (int)g_io.body_len;

    TEST_ASSERT_EQUAL_INT(0, mcp_handle_request(&g_req));
    // Bounded: exactly one attempt per retry budget.
    TEST_ASSERT_TRUE(g_io.recv_calls <= MCP_MAX_RECV_RETRIES + 1);
    TEST_ASSERT_EQUAL_INT(1, g_io.responses_sent);
}

TEST_CASE("queue full answers 503 without dropping the socket", "[mcp_stress]")
{
    mcp_setup();
    io_reset(NULL);
    install_transport();
    install_device_hooks_blocking();
    command_executor_deinit(); // deterministic start
    TEST_ASSERT_EQUAL_INT(ESP_OK, command_executor_init());

    // Blocking device hooks keep every dispatched job parked in its 2s ACK
    // wait, so workers and queue slots stay occupied deterministically.
    // Distinct device ids per job: same-device submissions would hit the
    // dispatcher's per-device BUSY rule and complete instantly instead.
    char device_call[256];
    const int capacity = CONFIG_CMD_EXEC_WORKER_COUNT + CONFIG_CMD_EXEC_QUEUE_LEN;
    for (int i = 0; i < capacity; i++) {
        snprintf(device_call, sizeof(device_call),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"device_command\","
                 "\"arguments\":{\"device_id\":\"relay-%d\","
                 "\"command\":\"toggle\"}}}", i, i);
        TEST_ASSERT_EQUAL_INT(0, run_request(device_call));
        // Accepted submissions answer nothing yet: the response comes from
        // the executor completion callback.
        TEST_ASSERT_EQUAL_INT(0, g_io.responses_sent);
    }

    // Capacity exceeded: refused synchronously with 503 on the same request.
    snprintf(device_call, sizeof(device_call),
             "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"device_command\","
             "\"arguments\":{\"device_id\":\"relay-overflow\","
             "\"command\":\"toggle\"}}}");
    TEST_ASSERT_EQUAL_INT(0, run_request(device_call));
    TEST_ASSERT_EQUAL_INT(1, g_io.responses_sent);
    TEST_ASSERT_TRUE(strstr(g_io.status_line, "503") != NULL);
    cJSON *response = cJSON_Parse(g_io.response);
    TEST_ASSERT_NOT_NULL(response);
    cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_TRUE(cJSON_IsNumber(
        cJSON_GetObjectItemCaseSensitive(error, "code")));
    cJSON_Delete(response);

    // Let the executor drain: queued jobs expire at the job deadline,
    // dispatched jobs return after the dispatcher ACK timeout.
    for (int i = 0; i < 700 && g_io.responses_sent < capacity + 1; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    TEST_ASSERT_TRUE(g_io.responses_sent >= capacity + 1);
    command_executor_deinit();
}
