#include <string.h>

#include "unity.h"

#include "esp_heap_caps.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_io.h"

static board_io_event_t s_last_event;
static int s_event_count;

static void capturing_handler(board_io_event_t event, void *context)
{
    (void)context;
    s_last_event = event;
    s_event_count++;
}

static void ensure_stopped(void)
{
    if (board_io_is_initialized()) {
        TEST_ASSERT_EQUAL(ESP_OK, board_io_deinit());
    }
    s_last_event = BOARD_IO_EVENT_COUNT;
    s_event_count = 0;
}

TEST_CASE("board io api rejected before init", "[board_io]")
{
    ensure_stopped();

    board_display_frame_t frame = {0};

    TEST_ASSERT_FALSE(board_io_is_initialized());
    TEST_ASSERT_EQUAL(ESP_OK, board_io_deinit());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      board_io_register_event_handler(capturing_handler, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, board_io_set_status(BOARD_STATUS_READY));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, board_io_signal(BOARD_SIGNAL_ACTIVITY));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, board_io_display_update(&frame));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, board_io_display_set_enabled(true));
}

TEST_CASE("board io init success and double init rejected", "[board_io]")
{
    ensure_stopped();

    TEST_ASSERT_EQUAL(ESP_OK, board_io_init());
    TEST_ASSERT_TRUE(board_io_is_initialized());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, board_io_init());

    TEST_ASSERT_EQUAL(ESP_OK, board_io_deinit());
    TEST_ASSERT_FALSE(board_io_is_initialized());
    TEST_ASSERT_EQUAL(ESP_OK, board_io_deinit());
}

TEST_CASE("board io reinit after deinit", "[board_io]")
{
    ensure_stopped();

    TEST_ASSERT_EQUAL(ESP_OK, board_io_init());
    TEST_ASSERT_EQUAL(ESP_OK, board_io_deinit());
    TEST_ASSERT_EQUAL(ESP_OK, board_io_init());
    TEST_ASSERT_TRUE(board_io_is_initialized());
}

TEST_CASE("board io handler registration rules", "[board_io]")
{
    ensure_stopped();

    TEST_ASSERT_EQUAL(ESP_OK, board_io_init());

    TEST_ASSERT_EQUAL(ESP_OK, board_io_register_event_handler(capturing_handler, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      board_io_register_event_handler(capturing_handler, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, board_io_register_event_handler(NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, board_io_register_event_handler(capturing_handler, NULL));

    TEST_ASSERT_EQUAL(ESP_OK, board_io_deinit());
    TEST_ASSERT_EQUAL(ESP_OK, board_io_init());
    TEST_ASSERT_EQUAL(ESP_OK, board_io_register_event_handler(capturing_handler, NULL));
}

TEST_CASE("board io invalid arguments while running", "[board_io]")
{
    ensure_stopped();
    TEST_ASSERT_EQUAL(ESP_OK, board_io_init());

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_io_set_status(BOARD_STATUS_COUNT));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_io_signal((board_signal_t)99));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_io_display_update(NULL));
}

TEST_CASE("board io display runtime disabled semantics", "[board_io]")
{
    ensure_stopped();
    TEST_ASSERT_EQUAL(ESP_OK, board_io_init());

    board_display_frame_t frame = {0};
    strcpy(frame.line[0], "PENDING");

    TEST_ASSERT_EQUAL(ESP_OK, board_io_display_set_enabled(false));
    TEST_ASSERT_EQUAL(ESP_OK, board_io_display_update(&frame));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, board_io_display_set_enabled(true));
    TEST_ASSERT_EQUAL(ESP_OK, board_io_display_set_enabled(false));
}

TEST_CASE("board io set status accepted while running", "[board_io]")
{
    ensure_stopped();
    TEST_ASSERT_EQUAL(ESP_OK, board_io_init());

    for (int s = 0; s < BOARD_STATUS_COUNT; s++) {
        TEST_ASSERT_EQUAL(ESP_OK, board_io_set_status((board_status_t)s));
    }
    TEST_ASSERT_EQUAL(ESP_OK, board_io_signal(BOARD_SIGNAL_ACTIVITY));
    TEST_ASSERT_EQUAL(ESP_OK, board_io_signal(BOARD_SIGNAL_IDENTIFY));
}

TEST_CASE("board io init deinit stress cycles without leak", "[board_io][stress]")
{
    ensure_stopped();

    TEST_ASSERT_EQUAL(ESP_OK, board_io_init());
    TEST_ASSERT_EQUAL(ESP_OK, board_io_deinit());

    size_t baseline = esp_get_free_heap_size();
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, board_io_init());
        TEST_ASSERT_TRUE(board_io_is_initialized());
        TEST_ASSERT_EQUAL(ESP_OK, board_io_deinit());
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    size_t final_free = esp_get_free_heap_size();
    TEST_ASSERT_TRUE(final_free + 2048 >= baseline);
    TEST_ASSERT_FALSE(board_io_is_initialized());
}

typedef struct {
    int iterations;
    SemaphoreHandle_t done;
} hammer_args_t;

static void status_hammer(void *arg)
{
    hammer_args_t *a = (hammer_args_t *)arg;
    for (int i = 0; i < a->iterations; i++) {
        board_status_t st = (i % 2 == 0) ? BOARD_STATUS_READY : BOARD_STATUS_WIFI_CONNECTING;
        (void)board_io_set_status(st);
        if (i % 10 == 0) {
            (void)board_io_signal(BOARD_SIGNAL_ACTIVITY);
        }
        vTaskDelay(1);
    }
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

static void display_hammer(void *arg)
{
    hammer_args_t *a = (hammer_args_t *)arg;
    board_display_frame_t frame;
    for (int i = 0; i < a->iterations; i++) {
        memset(&frame, 0, sizeof(frame));
        snprintf(frame.line[0], sizeof(frame.line[0]), "hammer %d", i);
        (void)board_io_display_update(&frame);
        vTaskDelay(1);
    }
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

TEST_CASE("board io concurrent api smoke", "[board_io][thread]")
{
    ensure_stopped();
    TEST_ASSERT_EQUAL(ESP_OK, board_io_init());

    SemaphoreHandle_t done_a = xSemaphoreCreateBinary();
    SemaphoreHandle_t done_b = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(done_a);
    TEST_ASSERT_NOT_NULL(done_b);

    static hammer_args_t args_a;
    static hammer_args_t args_b;
    args_a.iterations = 100;
    args_a.done = done_a;
    args_b.iterations = 100;
    args_b.done = done_b;

    TaskHandle_t ta = NULL;
    TaskHandle_t tb = NULL;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(status_hammer, "bio_hammer_a", 3072,
                                          &args_a, 3, &ta));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(display_hammer, "bio_hammer_b", 3072,
                                          &args_b, 3, &tb));

    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(done_a, pdMS_TO_TICKS(15000)));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(done_b, pdMS_TO_TICKS(15000)));

    vSemaphoreDelete(done_a);
    vSemaphoreDelete(done_b);

    TEST_ASSERT_TRUE(board_io_is_initialized());
    TEST_ASSERT_EQUAL(ESP_OK, board_io_deinit());
}
