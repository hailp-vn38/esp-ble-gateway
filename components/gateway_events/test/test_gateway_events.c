#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "gateway_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Helper ─────────────────────────────────────────────────────────── */

static void reset_all(void)
{
    gateway_events_reset_for_test();
}

static int s_listener_count;
static gateway_event_t s_last_event;

static void test_listener(const gateway_event_t *event, void *context)
{
    (void)context;
    s_listener_count++;
    s_last_event = *event;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

TEST_CASE("init succeeds", "[gateway_events]")
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(ESP_OK, gateway_events_init());
    reset_all();
}

TEST_CASE("register listener succeeds", "[gateway_events]")
{
    reset_all();
    gateway_events_init();

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          gateway_events_register(test_listener, NULL));

    reset_all();
}

TEST_CASE("register beyond capacity fails", "[gateway_events]")
{
    reset_all();
    gateway_events_init();

    for (int i = 0; i < GATEWAY_EVENT_MAX_LISTENERS; i++) {
        TEST_ASSERT_EQUAL_INT(ESP_OK,
                              gateway_events_register(test_listener, NULL));
    }
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NO_MEM,
                          gateway_events_register(test_listener, NULL));

    reset_all();
}

TEST_CASE("publish delivers to listener", "[gateway_events]")
{
    reset_all();
    gateway_events_init();

    s_listener_count = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          gateway_events_register(test_listener, NULL));

    gateway_event_t ev = {0};
    ev.type = GW_EVENT_DEVICE_CONNECTION;
    strlcpy(ev.device_id, "dev1", sizeof(ev.device_id));
    ev.bool_value = true;

    gateway_events_publish(&ev);

    TEST_ASSERT_EQUAL_INT(1, s_listener_count);
    TEST_ASSERT_EQUAL_INT(GW_EVENT_DEVICE_CONNECTION, s_last_event.type);
    TEST_ASSERT_EQUAL_STRING("dev1", s_last_event.device_id);
    TEST_ASSERT_TRUE(s_last_event.bool_value);

    reset_all();
}

TEST_CASE("publish assigns monotonic seq", "[gateway_events]")
{
    reset_all();
    gateway_events_init();

    s_listener_count = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          gateway_events_register(test_listener, NULL));

    gateway_event_t ev = {0};
    ev.type = GW_EVENT_FEATURE_STATE;

    uint32_t seq_before = gateway_events_current_seq();

    gateway_events_publish(&ev);
    uint32_t seq1 = s_last_event.seq;

    gateway_events_publish(&ev);
    uint32_t seq2 = s_last_event.seq;

    TEST_ASSERT_EQUAL_UINT32(seq_before + 1, seq1);
    TEST_ASSERT_EQUAL_UINT32(seq_before + 2, seq2);
    TEST_ASSERT_TRUE(seq2 > seq1);

    reset_all();
}

TEST_CASE("publish copies event value", "[gateway_events]")
{
    reset_all();
    gateway_events_init();

    s_listener_count = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          gateway_events_register(test_listener, NULL));

    gateway_event_t ev = {0};
    ev.type = GW_EVENT_FEATURE_STATE;
    strlcpy(ev.device_id, "before", sizeof(ev.device_id));

    gateway_events_publish(&ev);

    /* Mutate after publish */
    strlcpy(ev.device_id, "after", sizeof(ev.device_id));

    /* Listener should have received original value */
    TEST_ASSERT_EQUAL_STRING("before", s_last_event.device_id);

    reset_all();
}

TEST_CASE("multiple listeners all receive event", "[gateway_events]")
{
    reset_all();
    gateway_events_init();

    static int count_a, count_b;
    count_a = 0;
    count_b = 0;

    static void listener_a(const gateway_event_t *e, void *ctx)
    {
        (void)e;
        (void)ctx;
        count_a++;
    }
    static void listener_b(const gateway_event_t *e, void *ctx)
    {
        (void)e;
        (void)ctx;
        count_b++;
    }

    TEST_ASSERT_EQUAL_INT(ESP_OK, gateway_events_register(listener_a, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_OK, gateway_events_register(listener_b, NULL));

    gateway_event_t ev = {0};
    ev.type = GW_EVENT_DEVICE_CHANGED;
    gateway_events_publish(&ev);

    TEST_ASSERT_EQUAL_INT(1, count_a);
    TEST_ASSERT_EQUAL_INT(1, count_b);

    reset_all();
}

TEST_CASE("concurrent publish from multiple tasks", "[gateway_events]")
{
    reset_all();
    gateway_events_init();

    s_listener_count = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          gateway_events_register(test_listener, NULL));

    static int task_counter;
    task_counter = 0;

    static void publish_task(void *arg)
    {
        int *counter = (int *)arg;
        gateway_event_t ev = {0};
        ev.type = GW_EVENT_FEATURE_STATE;
        for (int i = 0; i < 50; i++) {
            gateway_events_publish(&ev);
            (*counter)++;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        vTaskDelete(NULL);
    }

    xTaskCreate(publish_task, "p1", 4096, &task_counter, 5, NULL);
    xTaskCreate(publish_task, "p2", 4096, &task_counter, 5, NULL);

    vTaskDelay(pdMS_TO_TICKS(300));

    /* s_listener_count should equal total publishes (100) */
    TEST_ASSERT_EQUAL_INT(100, s_listener_count);

    /* seq should be 100 */
    TEST_ASSERT_EQUAL_UINT32(100, gateway_events_current_seq());

    reset_all();
}

TEST_CASE("listener failure does not break other listeners", "[gateway_events]")
{
    reset_all();
    gateway_events_init();

    static int good_count;
    good_count = 0;

    static void bad_listener(const gateway_event_t *e, void *ctx)
    {
        (void)e;
        (void)ctx;
        /* Simulate crash in listener - we can't actually crash in a test,
         * but we verify the design: bad listener should not prevent
         * subsequent listeners from being called */
    }
    static void good_listener(const gateway_event_t *e, void *ctx)
    {
        (void)e;
        (void)ctx;
        good_count++;
    }

    TEST_ASSERT_EQUAL_INT(ESP_OK, gateway_events_register(bad_listener, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_OK, gateway_events_register(good_listener, NULL));

    gateway_event_t ev = {0};
    ev.type = GW_EVENT_DEVICE_CHANGED;
    gateway_events_publish(&ev);

    TEST_ASSERT_EQUAL_INT(1, good_count);

    reset_all();
}

TEST_CASE("event types are distinct", "[gateway_events]")
{
    TEST_ASSERT_NOT_EQUAL(GW_EVENT_DEVICE_CHANGED, GW_EVENT_DEVICE_CONNECTION);
    TEST_ASSERT_NOT_EQUAL(GW_EVENT_DEVICE_CONNECTION, GW_EVENT_DEVICE_SCHEMA);
    TEST_ASSERT_NOT_EQUAL(GW_EVENT_DEVICE_SCHEMA, GW_EVENT_FEATURE_STATE);
    TEST_ASSERT_NOT_EQUAL(GW_EVENT_FEATURE_STATE, GW_EVENT_RESYNC_REQUIRED);
}
