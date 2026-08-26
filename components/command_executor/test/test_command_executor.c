#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "unity.h"

#include "../../command_dispatcher/command_dispatcher_internal.h"
#include "command_dispatcher.h"
#include "command_executor.h"
#include "sdkconfig.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#define BLOCK_WAIT_MS       10000
#define COMPLETION_WAIT_MS  5000
#define EXPIRY_MARGIN_MS    300

typedef struct {
    SemaphoreHandle_t events;
    volatile int ok_count;
    volatile int timeout_count;
} tally_t;

static SemaphoreHandle_t s_gate;

static void cmd_exec_echo(const gw_message_t *message, dispatch_result_t *result)
{
    (void)message;
    command_dispatcher_set_text_result(result, DISPATCH_STATUS_OK, "echo");
}

static void cmd_exec_block(const gw_message_t *message, dispatch_result_t *result)
{
    (void)message;
    xSemaphoreTake(s_gate, pdMS_TO_TICKS(BLOCK_WAIT_MS));
    command_dispatcher_set_text_result(result, DISPATCH_STATUS_OK, "unblocked");
}

static void fresh_frozen_dispatcher(void)
{
    // Defensive cleanup: a previously aborted test may have leaked resources.
    command_executor_deinit();
    command_dispatcher_reset_for_test();
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_init());
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_register("exec_echo", cmd_exec_echo));
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_register("exec_block", cmd_exec_block));
    TEST_ASSERT_EQUAL_INT(0, command_dispatcher_freeze_registry());
    if (s_gate == NULL) {
        // Counting gate: every blocked handler must get its own token.
        s_gate = xSemaphoreCreateCounting(
            CONFIG_CMD_EXEC_WORKER_COUNT + CONFIG_CMD_EXEC_QUEUE_LEN + 1, 0);
        TEST_ASSERT_NOT_NULL(s_gate);
    }
    while (xSemaphoreTake(s_gate, 0) == pdTRUE) {} // drop stale tokens
}

static gw_message_t gateway_message(const char *command)
{
    gw_message_t message = {.protocol_version = GW_PROTOCOL_VERSION};
    strlcpy(message.type, "gateway_command", sizeof(message.type));
    strlcpy(message.command, command, sizeof(message.command));
    return message;
}

static void tally_completion(const dispatch_result_t *result, void *context)
{
    tally_t *tally = context;
    if (result->status == DISPATCH_STATUS_OK) {
        __atomic_fetch_add(&tally->ok_count, 1, __ATOMIC_RELAXED);
    } else if (result->status == DISPATCH_STATUS_TIMEOUT) {
        __atomic_fetch_add(&tally->timeout_count, 1, __ATOMIC_RELAXED);
    }
    xSemaphoreGive(tally->events);
}

static bool await_events(tally_t *tally, int expected_count)
{
    for (int i = 0; i < expected_count; i++) {
        if (xSemaphoreTake(tally->events, pdMS_TO_TICKS(COMPLETION_WAIT_MS)) !=
            pdTRUE) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("submit rejects invalid usage", "[command_executor]")
{
    gw_message_t message = gateway_message("exec_echo");

    command_executor_deinit();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      command_executor_submit(&message, tally_completion, NULL));

    TEST_ASSERT_EQUAL(ESP_OK, command_executor_init());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, command_executor_submit(NULL, tally_completion, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, command_executor_submit(&message, NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, command_executor_init());
    command_executor_deinit();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      command_executor_submit(&message, tally_completion, NULL));
}

TEST_CASE("submit runs a command and reports stats", "[command_executor]")
{
    fresh_frozen_dispatcher();
    TEST_ASSERT_EQUAL(ESP_OK, command_executor_init());

    tally_t tally = {.events = xSemaphoreCreateBinary()};
    TEST_ASSERT_NOT_NULL(tally.events);
    gw_message_t message = gateway_message("exec_echo");

    TEST_ASSERT_EQUAL(ESP_OK, command_executor_submit(&message, tally_completion, &tally));
    TEST_ASSERT_TRUE(await_events(&tally, 1));
    TEST_ASSERT_EQUAL_INT(1, tally.ok_count);

    command_executor_stats_t stats;
    uint32_t worker_stack_min = 0;
    command_executor_get_stats(&stats, &worker_stack_min);
    TEST_ASSERT_GREATER_THAN_UINT32(0, worker_stack_min);
    TEST_ASSERT_EQUAL_UINT32(1, stats.submitted);
    TEST_ASSERT_EQUAL_UINT32(1, stats.completed);
    TEST_ASSERT_EQUAL_UINT32(0, stats.queue_full);
    TEST_ASSERT_EQUAL_UINT32(0, stats.queue_timeout);

    vSemaphoreDelete(tally.events);
    command_executor_deinit();
}

TEST_CASE("queue-full submissions are rejected synchronously", "[command_executor]")
{
    fresh_frozen_dispatcher();
    TEST_ASSERT_EQUAL(ESP_OK, command_executor_init());

    tally_t tally = {.events = xSemaphoreCreateCounting(16, 0)};
    gw_message_t message = gateway_message("exec_block");

    // Workers + queue capacity: all accepted, the next one must fail.
    const int capacity = CONFIG_CMD_EXEC_WORKER_COUNT + CONFIG_CMD_EXEC_QUEUE_LEN;
    for (int i = 0; i < capacity; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, command_executor_submit(&message, tally_completion, &tally));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, command_executor_submit(&message, tally_completion, &tally));

    // Unblock the handlers so workers drain the backlog.
    for (int i = 0; i < capacity; i++) xSemaphoreGive(s_gate);
    TEST_ASSERT_TRUE(await_events(&tally, capacity));
    TEST_ASSERT_EQUAL_INT(capacity, tally.ok_count);
    TEST_ASSERT_EQUAL_INT(0, tally.timeout_count);

    command_executor_stats_t stats;
    uint32_t worker_stack_min = 0;
    command_executor_get_stats(&stats, &worker_stack_min);
    TEST_ASSERT_GREATER_THAN_UINT32(0, worker_stack_min);
    // Rejected submissions are not counted as submitted.
    TEST_ASSERT_EQUAL_UINT32((uint32_t)capacity, stats.submitted);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)capacity, stats.completed);
    TEST_ASSERT_EQUAL_UINT32(1, stats.queue_full);

    vSemaphoreDelete(tally.events);
    command_executor_deinit();
}

TEST_CASE("expired jobs complete with timeout without dispatching", "[command_executor]")
{
    fresh_frozen_dispatcher();
    TEST_ASSERT_EQUAL(ESP_OK, command_executor_init());

    tally_t tally = {.events = xSemaphoreCreateCounting(16, 0)};
    gw_message_t message = gateway_message("exec_block");

    const int capacity = CONFIG_CMD_EXEC_WORKER_COUNT + CONFIG_CMD_EXEC_QUEUE_LEN;
    for (int i = 0; i < capacity; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, command_executor_submit(&message, tally_completion, &tally));
    }

    // Let every job's deadline pass while workers sit blocked in the handler.
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CMD_EXEC_JOB_TIMEOUT_MS + EXPIRY_MARGIN_MS));

    // Release only the in-flight handlers: queued jobs must expire instead of
    // being dispatched.
    for (int i = 0; i < CONFIG_CMD_EXEC_WORKER_COUNT; i++) xSemaphoreGive(s_gate);
    TEST_ASSERT_TRUE(await_events(&tally, capacity));
    TEST_ASSERT_EQUAL_INT(CONFIG_CMD_EXEC_WORKER_COUNT, tally.ok_count);
    TEST_ASSERT_EQUAL_INT(CONFIG_CMD_EXEC_QUEUE_LEN, tally.timeout_count);

    command_executor_stats_t stats;
    command_executor_get_stats(&stats, NULL);
    TEST_ASSERT_EQUAL_UINT32(CONFIG_CMD_EXEC_QUEUE_LEN, stats.queue_timeout);
    TEST_ASSERT_EQUAL_UINT32(0, stats.dispatch_timeout);

    vSemaphoreDelete(tally.events);
    command_executor_deinit();
}
