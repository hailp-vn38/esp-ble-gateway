#include <string.h>

#include "unity.h"

#include "board_display.h"
#include "board_io.h"

#if CONFIG_BOARD_IO_DISPLAY_ENABLE

static int s_render_calls;
static esp_err_t s_render_rc;
static board_display_frame_t s_last_rendered;

static esp_err_t fake_init(void)
{
    return ESP_OK;
}

static void fake_deinit(void)
{
}

static esp_err_t fake_set_enabled(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}

static esp_err_t fake_render(const board_display_frame_t *frame)
{
    s_render_calls++;
    if (s_render_rc != ESP_OK) {
        return s_render_rc;
    }
    s_last_rendered = *frame;
    return ESP_OK;
}

static const board_display_backend_t FAKE_BACKEND = {
    .init = fake_init,
    .deinit = fake_deinit,
    .set_enabled = fake_set_enabled,
    .render = fake_render,
};

static bool s_module_inited;

static void ensure_inited(void)
{
    if (!s_module_inited) {
        TEST_ASSERT_EQUAL(ESP_OK, board_display_init());
        s_module_inited = true;
    }
    board_display_test_reset();
    board_display_test_set_backend(&FAKE_BACKEND);
    s_render_calls = 0;
    s_render_rc = ESP_OK;
    memset(&s_last_rendered, 0, sizeof(s_last_rendered));
}

TEST_CASE("display frame is copied at update time", "[board_io][dsp]")
{
    ensure_inited();

    board_display_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    strcpy(frame.line[0], "ORIGINAL");

    TEST_ASSERT_EQUAL(ESP_OK, board_display_update(&frame));

    strcpy(frame.line[0], "MUTATED");

    board_display_process(1000);

    TEST_ASSERT_EQUAL_INT(1, s_render_calls);
    TEST_ASSERT_EQUAL_STRING("ORIGINAL", s_last_rendered.line[0]);
}

TEST_CASE("display forces NUL termination on full lines", "[board_io][dsp]")
{
    ensure_inited();

    board_display_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    memset(frame.line[0], 'X', BOARD_IO_DISPLAY_LINE_LEN);
    memcpy(frame.line[1], "01234567890123456789012345678901", BOARD_IO_DISPLAY_LINE_LEN);

    TEST_ASSERT_EQUAL(ESP_OK, board_display_update(&frame));
    board_display_process(1000);

    TEST_ASSERT_EQUAL_INT(1, s_render_calls);
    TEST_ASSERT_EQUAL('\0', s_last_rendered.line[0][BOARD_IO_DISPLAY_LINE_LEN - 1]);
    TEST_ASSERT_EQUAL('\0', s_last_rendered.line[1][BOARD_IO_DISPLAY_LINE_LEN - 1]);
    TEST_ASSERT_EQUAL_INT(BOARD_IO_DISPLAY_LINE_LEN - 1,
                          (int)strnlen(s_last_rendered.line[1], BOARD_IO_DISPLAY_LINE_LEN));
}

TEST_CASE("display coalesces burst to latest and caps refresh", "[board_io][dsp]")
{
    ensure_inited();

    board_display_frame_t a, b, c;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    strcpy(a.line[0], "A");
    strcpy(b.line[0], "B");
    strcpy(c.line[0], "C");

    TEST_ASSERT_EQUAL(ESP_OK, board_display_update(&a));
    board_display_process(1000);
    TEST_ASSERT_EQUAL_INT(1, s_render_calls);

    TEST_ASSERT_EQUAL(ESP_OK, board_display_update(&b));
    TEST_ASSERT_EQUAL(ESP_OK, board_display_update(&c));

    board_display_process(1020);
    board_display_process(1100);
    board_display_process(1199);
    TEST_ASSERT_EQUAL_INT(1, s_render_calls);

    board_display_process(1200);
    TEST_ASSERT_EQUAL_INT(2, s_render_calls);
    TEST_ASSERT_EQUAL_STRING("C", s_last_rendered.line[0]);
}

TEST_CASE("display runtime disable holds latest pending", "[board_io][dsp]")
{
    ensure_inited();

    board_display_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    strcpy(frame.line[0], "HELD");

    TEST_ASSERT_EQUAL(ESP_OK, board_display_set_runtime_enabled(false));
    TEST_ASSERT_EQUAL(ESP_OK, board_display_update(&frame));

    board_display_process(1000);
    TEST_ASSERT_EQUAL_INT(0, s_render_calls);

    TEST_ASSERT_EQUAL(ESP_OK, board_display_set_runtime_enabled(true));
    board_display_process(1010);
    TEST_ASSERT_EQUAL_INT(1, s_render_calls);
    TEST_ASSERT_EQUAL_STRING("HELD", s_last_rendered.line[0]);
}

TEST_CASE("display enable without backend is unsupported", "[board_io][dsp]")
{
    ensure_inited();
    board_display_test_reset();

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, board_display_set_runtime_enabled(true));
}

TEST_CASE("display backend failure does not kill pipeline", "[board_io][dsp]")
{
    ensure_inited();

    board_display_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    strcpy(frame.line[0], "ERRCASE");

    s_render_rc = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, board_display_update(&frame));
    board_display_process(1000);
    TEST_ASSERT_EQUAL_INT(1, s_render_calls);

    uint64_t next_allowed = 0;
    TEST_ASSERT_FALSE(board_display_wants_render(1050, &next_allowed));

    s_render_rc = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, board_display_update(&frame));
    board_display_process(1300);
    TEST_ASSERT_EQUAL_INT(2, s_render_calls);
}

#else

TEST_CASE("display compile-disabled returns NOT_SUPPORTED", "[board_io][dsp]")
{
    TEST_ASSERT_FALSE(board_display_capability_enabled());

    board_display_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, board_io_display_update(&frame));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, board_io_display_set_enabled(true));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, board_io_display_set_enabled(false));
}

#endif
