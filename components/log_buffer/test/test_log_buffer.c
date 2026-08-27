#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "log_buffer.h"
#include "unity.h"

#define PRODUCER_TASK_COUNT 4
#define PRODUCER_STACK_SIZE 3072U
#define JOIN_TIMEOUT_MS     10000U

static void suite_reset(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, log_buffer_init());
    log_buffer_clear();
    log_buffer_test_reset_metrics();
}

static void fill_entry_name(char *out, size_t out_size, unsigned int index)
{
    snprintf(out, out_size, "e%05u", index);
}

TEST_CASE("log_buffer basic push keeps chronological order", "[log_buffer]")
{
    suite_reset();

    TEST_ASSERT_TRUE(log_buffer_push("A"));
    TEST_ASSERT_TRUE(log_buffer_push("B"));
    TEST_ASSERT_TRUE(log_buffer_push("C"));

    log_entry_t entries[4];
    int count = log_buffer_get_recent(entries, 4);
    TEST_ASSERT_EQUAL_INT(3, count);
    TEST_ASSERT_EQUAL_STRING("A", entries[0].text);
    TEST_ASSERT_EQUAL_STRING("B", entries[1].text);
    TEST_ASSERT_EQUAL_STRING("C", entries[2].text);

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT64(now_ms, entries[i].uptime_ms);
    }
}

TEST_CASE("log_buffer returns only the most recent requested entries", "[log_buffer]")
{
    suite_reset();

    for (unsigned int i = 0; i < 30U; i++) {
        char name[16];
        fill_entry_name(name, sizeof(name), i);
        TEST_ASSERT_TRUE(log_buffer_push(name));
    }

    log_entry_t recent[5];
    TEST_ASSERT_EQUAL_INT(5, log_buffer_get_recent(recent, 5));
    TEST_ASSERT_EQUAL_STRING("e00025", recent[0].text);
    TEST_ASSERT_EQUAL_STRING("e00026", recent[1].text);
    TEST_ASSERT_EQUAL_STRING("e00029", recent[4].text);
}

TEST_CASE("log_buffer empty read returns zero", "[log_buffer]")
{
    suite_reset();

    log_entry_t entries[2];
    TEST_ASSERT_EQUAL_INT(0, log_buffer_get_recent(entries, 2));
}

TEST_CASE("log_buffer recent API validates its arguments", "[log_buffer]")
{
    suite_reset();
    TEST_ASSERT_TRUE(log_buffer_push("present"));

    log_entry_t entry;
    TEST_ASSERT_EQUAL_INT(-1, log_buffer_get_recent(&entry, 0));
    TEST_ASSERT_EQUAL_INT(-1, log_buffer_get_recent(NULL, 1));
}

TEST_CASE("log_buffer handles exactly full capacity", "[log_buffer]")
{
    suite_reset();

    for (unsigned int i = 0; i < LOG_BUFFER_CAPACITY; i++) {
        char name[16];
        fill_entry_name(name, sizeof(name), i);
        TEST_ASSERT_TRUE(log_buffer_push(name));
    }

    /* 64 x 200B = 12.8KB: must be static, the main task stack cannot hold it. */
    static log_entry_t entries[LOG_BUFFER_CAPACITY];
    int count = log_buffer_get_recent(entries, LOG_BUFFER_CAPACITY);
    TEST_ASSERT_EQUAL_INT((int)LOG_BUFFER_CAPACITY, count);
    TEST_ASSERT_EQUAL_STRING("e00000", entries[0].text);

    char last_name[16];
    fill_entry_name(last_name, sizeof(last_name), LOG_BUFFER_CAPACITY - 1U);
    TEST_ASSERT_EQUAL_STRING(last_name, entries[LOG_BUFFER_CAPACITY - 1U].text);

    for (size_t i = 1; i < LOG_BUFFER_CAPACITY; i++) {
        TEST_ASSERT_TRUE(strcmp(entries[i - 1U].text, entries[i].text) < 0);
    }
}

TEST_CASE("log_buffer overwrites oldest entry past capacity", "[log_buffer]")
{
    suite_reset();

    for (unsigned int i = 0; i < LOG_BUFFER_CAPACITY + 1U; i++) {
        char name[16];
        fill_entry_name(name, sizeof(name), i);
        TEST_ASSERT_TRUE(log_buffer_push(name));
    }

    static log_entry_t entries[LOG_BUFFER_CAPACITY];
    int count = log_buffer_get_recent(entries, LOG_BUFFER_CAPACITY);
    TEST_ASSERT_EQUAL_INT((int)LOG_BUFFER_CAPACITY, count);
    TEST_ASSERT_EQUAL_STRING("e00001", entries[0].text);

    char last_name[16];
    fill_entry_name(last_name, sizeof(last_name), LOG_BUFFER_CAPACITY);
    TEST_ASSERT_EQUAL_STRING(last_name, entries[LOG_BUFFER_CAPACITY - 1U].text);
}

TEST_CASE("log_buffer survives multiple wraps in order", "[log_buffer]")
{
    suite_reset();

    const unsigned int total = 2U * LOG_BUFFER_CAPACITY + 7U;
    for (unsigned int i = 0; i < total; i++) {
        char name[16];
        fill_entry_name(name, sizeof(name), i);
        TEST_ASSERT_TRUE(log_buffer_push(name));
    }

    static log_entry_t entries[LOG_BUFFER_CAPACITY];
    int count = log_buffer_get_recent(entries, LOG_BUFFER_CAPACITY);
    TEST_ASSERT_EQUAL_INT((int)LOG_BUFFER_CAPACITY, count);

    unsigned int first_kept = total - LOG_BUFFER_CAPACITY;
    char first_name[16];
    fill_entry_name(first_name, sizeof(first_name), first_kept);
    TEST_ASSERT_EQUAL_STRING(first_name, entries[0].text);

    char last_name[16];
    fill_entry_name(last_name, sizeof(last_name), total - 1U);
    TEST_ASSERT_EQUAL_STRING(last_name, entries[LOG_BUFFER_CAPACITY - 1U].text);

    for (size_t i = 1; i < LOG_BUFFER_CAPACITY; i++) {
        TEST_ASSERT_TRUE(strcmp(entries[i - 1U].text, entries[i].text) < 0);
    }
}

TEST_CASE("log_buffer bounded copy never writes past requested capacity", "[log_buffer]")
{
    suite_reset();

    for (unsigned int i = 0; i < LOG_BUFFER_CAPACITY; i++) {
        char name[16];
        fill_entry_name(name, sizeof(name), i);
        TEST_ASSERT_TRUE(log_buffer_push(name));
    }

    static log_entry_t frame[5];
    log_entry_t *canary = &frame[4];

    log_entry_t expected_canary;
    memset(&expected_canary, 0, sizeof(expected_canary));
    memset(expected_canary.text, 0xA5, sizeof(expected_canary.text));
    expected_canary.uptime_ms = 0xDEADBEEFCAFEBULL;
    memcpy(canary, &expected_canary, sizeof(expected_canary));

    int count = log_buffer_get_recent(frame, 4);
    TEST_ASSERT_EQUAL_INT(4, count);

    char first_name[16];
    fill_entry_name(first_name, sizeof(first_name), LOG_BUFFER_CAPACITY - 4U);
    char last_name[16];
    fill_entry_name(last_name, sizeof(last_name), LOG_BUFFER_CAPACITY - 1U);
    TEST_ASSERT_EQUAL_STRING(first_name, frame[0].text);
    TEST_ASSERT_EQUAL_STRING(last_name, frame[3].text);
    TEST_ASSERT_EQUAL_MEMORY(&expected_canary, canary, sizeof(expected_canary));
}

TEST_CASE("log_buffer truncates long input at max length", "[log_buffer]")
{
    suite_reset();

    char input[250];
    for (size_t i = 0; i < sizeof(input) - 1U; i++) {
        input[i] = (char)('a' + (int)(i % 26U));
    }
    input[sizeof(input) - 1U] = '\0';

    TEST_ASSERT_TRUE(log_buffer_push(input));

    log_entry_t entry;
    TEST_ASSERT_EQUAL_INT(1, log_buffer_get_recent(&entry, 1));
    TEST_ASSERT_EQUAL_INT((int)(LOG_ENTRY_MAX_LEN - 1U),
                          (int)strnlen(entry.text, LOG_ENTRY_MAX_LEN));
    TEST_ASSERT_EQUAL_CHAR('\0', entry.text[LOG_ENTRY_MAX_LEN - 1U]);
    TEST_ASSERT_EQUAL_MEMORY(input, entry.text, LOG_ENTRY_MAX_LEN - 1U);
}

TEST_CASE("log_buffer trims trailing CR and LF", "[log_buffer]")
{
    suite_reset();

    TEST_ASSERT_TRUE(log_buffer_push("abc\n"));
    TEST_ASSERT_TRUE(log_buffer_push("abc\r"));
    TEST_ASSERT_TRUE(log_buffer_push("abc\r\n"));

    log_entry_t entries[4];
    int count = log_buffer_get_recent(entries, 4);
    TEST_ASSERT_EQUAL_INT(3, count);
    TEST_ASSERT_EQUAL_STRING("abc", entries[0].text);
    TEST_ASSERT_EQUAL_STRING("abc", entries[1].text);
    TEST_ASSERT_EQUAL_STRING("abc", entries[2].text);
}

TEST_CASE("log_buffer rejects null empty and blank input", "[log_buffer]")
{
    suite_reset();

    TEST_ASSERT_FALSE(log_buffer_push(NULL));
    TEST_ASSERT_FALSE(log_buffer_push(""));
    TEST_ASSERT_FALSE(log_buffer_push("\r\n"));

    log_entry_t entries[2];
    TEST_ASSERT_EQUAL_INT(0, log_buffer_get_recent(entries, 2));
    TEST_ASSERT_EQUAL_UINT32(0, log_buffer_get_dropped_count());
}

TEST_CASE("log_buffer repeated init preserves entries and hook", "[log_buffer]")
{
    suite_reset();
    TEST_ASSERT_TRUE(log_buffer_push("keep-A"));

    TEST_ASSERT_EQUAL(ESP_OK, log_buffer_init());

    log_entry_t entries[LOG_BUFFER_CAPACITY];
    int count = log_buffer_get_recent(entries, LOG_BUFFER_CAPACITY);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_STRING("keep-A", entries[0].text);

    ESP_LOGI("lb_test", "post-reinit-capture");
    count = log_buffer_get_recent(entries, LOG_BUFFER_CAPACITY);
    TEST_ASSERT_GREATER_THAN(1, count);
    TEST_ASSERT_NOT_NULL(strstr(entries[count - 1].text, "post-reinit-capture"));
}

TEST_CASE("log_buffer clear empties ring but keeps capturing", "[log_buffer]")
{
    suite_reset();
    TEST_ASSERT_TRUE(log_buffer_push("before-clear-1"));
    TEST_ASSERT_TRUE(log_buffer_push("before-clear-2"));

    log_buffer_clear();

    log_entry_t entries[LOG_BUFFER_CAPACITY];
    TEST_ASSERT_EQUAL_INT(0, log_buffer_get_recent(entries, LOG_BUFFER_CAPACITY));

    ESP_LOGI("lb_test", "post-clear-capture");
    int count = log_buffer_get_recent(entries, LOG_BUFFER_CAPACITY);
    TEST_ASSERT_GREATER_THAN(0, count);
    TEST_ASSERT_NOT_NULL(strstr(entries[count - 1].text, "post-clear-capture"));
}

TEST_CASE("log_buffer counts drop on writer contention", "[log_buffer]")
{
    suite_reset();

    SemaphoreHandle_t ring_mutex = log_buffer_test_get_ring_mutex();
    TEST_ASSERT_NOT_NULL(ring_mutex);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(ring_mutex, pdMS_TO_TICKS(100)));

    TEST_ASSERT_FALSE(log_buffer_push("contended"));
    TEST_ASSERT_EQUAL_UINT32(1, log_buffer_get_dropped_count());

    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreGive(ring_mutex));

    TEST_ASSERT_TRUE(log_buffer_push("after-release"));
    TEST_ASSERT_EQUAL_UINT32(1, log_buffer_get_dropped_count());

    log_entry_t entry;
    TEST_ASSERT_EQUAL_INT(1, log_buffer_get_recent(&entry, 1));
    TEST_ASSERT_EQUAL_STRING("after-release", entry.text);
}

typedef struct {
    int task_id;
    int iterations;
} producer_ctx_t;

static SemaphoreHandle_t s_join_sem;
static producer_ctx_t s_producer_ctx[PRODUCER_TASK_COUNT];

static void direct_producer_task(void *arg)
{
    producer_ctx_t *ctx = (producer_ctx_t *)arg;
    char line[32];
    for (int i = 0; i < ctx->iterations; i++) {
        snprintf(line, sizeof(line), "t%d-%04d", ctx->task_id, i);
        (void)log_buffer_push(line);
    }
    xSemaphoreGive(s_join_sem);
    vTaskDelete(NULL);
}

static void logger_producer_task(void *arg)
{
    producer_ctx_t *ctx = (producer_ctx_t *)arg;
    for (int i = 0; i < ctx->iterations; i++) {
        ESP_LOGI("lb_reent", "task=%d entry=%d", ctx->task_id, i);
    }
    xSemaphoreGive(s_join_sem);
    vTaskDelete(NULL);
}

static void run_concurrent_producers(TaskFunction_t task_fn, int iterations)
{
    s_join_sem = xSemaphoreCreateCounting(PRODUCER_TASK_COUNT, 0);
    TEST_ASSERT_NOT_NULL(s_join_sem);

    for (int t = 0; t < PRODUCER_TASK_COUNT; t++) {
        s_producer_ctx[t].task_id = t;
        s_producer_ctx[t].iterations = iterations;
        TEST_ASSERT_EQUAL(pdPASS,
                          xTaskCreate(task_fn, "lb_prod", PRODUCER_STACK_SIZE,
                                      &s_producer_ctx[t], tskIDLE_PRIORITY + 2U, NULL));
    }
    for (int t = 0; t < PRODUCER_TASK_COUNT; t++) {
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_join_sem, pdMS_TO_TICKS(JOIN_TIMEOUT_MS)));
    }
    vSemaphoreDelete(s_join_sem);
    s_join_sem = NULL;
}

static void assert_ring_valid_and_contains(const char *needle)
{
    static log_entry_t entries[LOG_BUFFER_CAPACITY];
    int count = log_buffer_get_recent(entries, LOG_BUFFER_CAPACITY);
    TEST_ASSERT_GREATER_THAN(0, count);
    TEST_ASSERT_LESS_OR_EQUAL_INT((int)LOG_BUFFER_CAPACITY, count);

    bool found = false;
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_NOT_NULL(memchr(entries[i].text, '\0', LOG_ENTRY_MAX_LEN));
        TEST_ASSERT_LESS_OR_EQUAL_INT((int)(LOG_ENTRY_MAX_LEN - 1U),
                                      (int)strnlen(entries[i].text, LOG_ENTRY_MAX_LEN));
        if (strstr(entries[i].text, needle) != NULL) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
}

TEST_CASE("log_buffer survives direct multi-producer contention", "[log_buffer][concurrency]")
{
    suite_reset();

    run_concurrent_producers(direct_producer_task, 60);

    TEST_ASSERT_TRUE(log_buffer_push("marker-final"));
    assert_ring_valid_and_contains("marker-final");
}

TEST_CASE("log_buffer survives concurrent ESP_LOGI through global hook",
          "[log_buffer][concurrency]")
{
    suite_reset();

    run_concurrent_producers(logger_producer_task, 25);

    TEST_ASSERT_TRUE(log_buffer_push("marker-final"));
    assert_ring_valid_and_contains("reent");
}
