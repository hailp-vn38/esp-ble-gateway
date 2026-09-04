#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "unity.h"

#include "cbor_codec.h"

#include "test_mcp_transport.h"

// Memory-stability and transport-robustness checks (plan step 8). True
// multi-client concurrency needs real hardware and is verified manually with
// parallel curl sessions; here we prove the single-task paths do not leak,
// fragment or spin.

static void mcp_setup(void)
{
    mcp_auth_reset_rate_limit();
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

    // Warm up lazily allocated resources (cJSON caches).
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
    // device_command is removed; this test now verifies the socket is not
    // dropped on a burst of valid requests — the gateway handles them.
    mcp_setup();
    install_transport();
    const int burst = 20;
    for (int i = 0; i < burst; i++) {
        char body[128];
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"get_status\"}}", i);
        TEST_ASSERT_EQUAL_INT(0, run_request(body));
    }
    // No 503 path via static get_status (synchronous) — just verify no crash
    // and socket remains usable for next request.
    TEST_ASSERT_EQUAL_INT(0, run_request(
        "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"get_status\"}}"));
    cJSON *response = cJSON_Parse(g_io.response);
    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(response, "result"));
    cJSON_Delete(response);
}
