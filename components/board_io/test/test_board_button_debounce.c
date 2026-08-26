#include <stdbool.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_button.h"

#define DEBOUNCE_MS 40U
#define RESTART_MS 2000U
#define FACTORY_MS 8000U
#define TEST_BUTTON_GPIO 4

static int s_fake_level;

static int fake_reader(int gpio)
{
    (void)gpio;
    return s_fake_level;
}

static board_button_config_t debounce_suite_begin(void)
{
    s_fake_level = 1;
    board_button_config_t cfg;
    cfg.gpio = TEST_BUTTON_GPIO;
    cfg.active_low = true;
    cfg.pull = BOARD_PIN_PULL_UP;
    cfg.debounce_ms = DEBOUNCE_MS;
    cfg.restart_ms = RESTART_MS;
    cfg.factory_ms = FACTORY_MS;
    TEST_ASSERT_EQUAL(ESP_OK,
                      board_button_init(&cfg, xTaskGetCurrentTaskHandle()));
    return cfg;
}

static void press_level(int level)
{
    s_fake_level = level;
    board_button_on_edge_notify();
}

TEST_CASE("debounce single stable press emits one short press", "[board_io][debounce]")
{
    (void)debounce_suite_begin();
    board_io_event_t event;
    size_t n;

    press_level(0);
    n = board_button_process(1000, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(0, n);

    n = board_button_process(1039, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(0, n);

    uint64_t dl = 0;
    TEST_ASSERT_TRUE(board_button_next_deadline(1039, &dl));
    TEST_ASSERT_EQUAL_UINT64(1040, dl);

    n = board_button_process(1040, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(0, n);

    press_level(1);
    (void)board_button_process(1100, fake_reader, &event, 1);
    n = board_button_process(1140, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL(BOARD_IO_EVENT_BUTTON_SHORT_PRESS, event);

    board_button_deinit();
}

TEST_CASE("debounce bounce burst collapses into single logical press", "[board_io][debounce]")
{
    (void)debounce_suite_begin();
    board_io_event_t event;
    const uint64_t edges[] = {1000, 1005, 1008, 1012, 1018};

    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        board_button_on_edge_notify();
        (void)board_button_process(edges[i], fake_reader, &event, 1);
    }

    uint64_t dl = 0;
    TEST_ASSERT_TRUE(board_button_next_deadline(1057, &dl));
    TEST_ASSERT_EQUAL_UINT64(1058, dl);

    s_fake_level = 0;
    size_t n = board_button_process(1058, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(0, n);

    s_fake_level = 1;
    board_button_on_edge_notify();
    (void)board_button_process(1100, fake_reader, &event, 1);
    n = board_button_process(1140, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL(BOARD_IO_EVENT_BUTTON_SHORT_PRESS, event);

    board_button_deinit();
}

TEST_CASE("debounce does not commit before quiet window ends", "[board_io][debounce]")
{
    (void)debounce_suite_begin();
    board_io_event_t event;

    press_level(0);
    for (uint64_t t = 1000; t < 1040; t += 10) {
        size_t n = board_button_process(t, fake_reader, &event, 1);
        TEST_ASSERT_EQUAL_INT(0, n);
        uint64_t dl = 0;
        TEST_ASSERT_TRUE(board_button_next_deadline(t, &dl));
        TEST_ASSERT_TRUE(dl > t);
    }

    board_button_deinit();
}

TEST_CASE("debounce release bounce yields single physical release", "[board_io][debounce]")
{
    (void)debounce_suite_begin();
    board_io_event_t event;

    s_fake_level = 0;
    board_button_on_edge_notify();
    (void)board_button_process(0, fake_reader, &event, 1);
    (void)board_button_process(DEBOUNCE_MS, fake_reader, &event, 1);

    int events_during_bounce = 0;
    const uint64_t release_edges[] = {5000, 5005, 5012};
    for (size_t i = 0; i < sizeof(release_edges) / sizeof(release_edges[0]); i++) {
        s_fake_level = 1;
        board_button_on_edge_notify();
        if (board_button_process(release_edges[i], fake_reader, &event, 1) == 1) {
            events_during_bounce++;
        }
    }
    size_t settled = board_button_process(5052, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(0, events_during_bounce);
    TEST_ASSERT_EQUAL_INT(1, settled);

    board_button_deinit();
}

TEST_CASE("debounce short glitch produces no semantic action", "[board_io][debounce]")
{
    (void)debounce_suite_begin();
    board_io_event_t event;

    s_fake_level = 0;
    board_button_on_edge_notify();
    (void)board_button_process(0, fake_reader, &event, 1);

    s_fake_level = 1;
    board_button_on_edge_notify();
    (void)board_button_process(5, fake_reader, &event, 1);

    size_t n = board_button_process(DEBOUNCE_MS + 5, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(0, n);

    uint64_t dl = 0;
    TEST_ASSERT_FALSE(board_button_next_deadline(DEBOUNCE_MS + 6, &dl));

    s_fake_level = 0;
    board_button_on_edge_notify();
    (void)board_button_process(10000, fake_reader, &event, 1);
    (void)board_button_process(10040, fake_reader, &event, 1);
    s_fake_level = 1;
    board_button_on_edge_notify();
    (void)board_button_process(10100, fake_reader, &event, 1);
    n = board_button_process(10140, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(1, n);

    board_button_deinit();
}

TEST_CASE("edge storm coalesces without overflow", "[board_io][debounce]")
{
    (void)debounce_suite_begin();
    board_io_event_t event;

    for (int i = 0; i < 1000; i++) {
        board_button_on_edge_notify();
        (void)board_button_process((uint64_t)i, fake_reader, &event, 1);
    }

    s_fake_level = 0;
    board_button_on_edge_notify();
    (void)board_button_process(2000, fake_reader, &event, 1);
    (void)board_button_process(2040, fake_reader, &event, 1);
    s_fake_level = 1;
    board_button_on_edge_notify();
    (void)board_button_process(2100, fake_reader, &event, 1);
    size_t n = board_button_process(2140, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(1, n);

    board_button_deinit();
}

TEST_CASE("module classifies exact restart hold through release", "[board_io][debounce]")
{
    (void)debounce_suite_begin();
    board_io_event_t event;

    s_fake_level = 0;
    board_button_on_edge_notify();
    (void)board_button_process(1000, fake_reader, &event, 1);
    (void)board_button_process(1040, fake_reader, &event, 1);
    (void)board_button_process(3100, fake_reader, &event, 1);

    s_fake_level = 1;
    board_button_on_edge_notify();
    (void)board_button_process(3200, fake_reader, &event, 1);
    size_t n = board_button_process(3240, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL(BOARD_IO_EVENT_RESTART_REQUEST, event);

    board_button_deinit();
}

TEST_CASE("module classifies exact factory hold through release", "[board_io][debounce]")
{
    (void)debounce_suite_begin();
    board_io_event_t event;

    s_fake_level = 0;
    board_button_on_edge_notify();
    (void)board_button_process(1000, fake_reader, &event, 1);
    (void)board_button_process(1040, fake_reader, &event, 1);
    (void)board_button_process(9100, fake_reader, &event, 1);

    s_fake_level = 1;
    board_button_on_edge_notify();
    (void)board_button_process(9200, fake_reader, &event, 1);
    size_t n = board_button_process(9240, fake_reader, &event, 1);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL(BOARD_IO_EVENT_FACTORY_RESET_REQUEST, event);

    board_button_deinit();
}
