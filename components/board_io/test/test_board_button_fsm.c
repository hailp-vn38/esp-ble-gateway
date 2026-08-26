#include <stdbool.h>

#include "unity.h"

#include "board_button_fsm.h"

static const uint32_t RESTART_MS = 2000U;
static const uint32_t FACTORY_MS = 8000U;

static void fsm_fresh(board_button_fsm_t *fsm)
{
    board_button_fsm_init(fsm);
}

TEST_CASE("button fsm starts released with no event", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);

    board_button_fsm_result_t r =
        board_button_fsm_feed(&fsm, false, 100, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_FALSE(r.has_event);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_NONE, r.overlay);
    TEST_ASSERT_EQUAL(BOARD_BUTTON_FSM_RELEASED, fsm.state);
}

TEST_CASE("button fsm stable press enters PRESSED without event", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);

    board_button_fsm_result_t r =
        board_button_fsm_feed(&fsm, true, 100, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_FALSE(r.has_event);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_NONE, r.overlay);
    TEST_ASSERT_EQUAL(BOARD_BUTTON_FSM_PRESSED, fsm.state);
    TEST_ASSERT_EQUAL_UINT64(100, fsm.press_start_ms);
}

TEST_CASE("button fsm short press emits exactly one SHORT_PRESS", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);
    (void)board_button_fsm_feed(&fsm, true, 100, RESTART_MS, FACTORY_MS);

    board_button_fsm_result_t r =
        board_button_fsm_feed(&fsm, false, 100 + RESTART_MS - 1, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_TRUE(r.has_event);
    TEST_ASSERT_EQUAL(BOARD_IO_EVENT_BUTTON_SHORT_PRESS, r.event);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_NONE, r.overlay);

    board_button_fsm_result_t dup =
        board_button_fsm_feed(&fsm, false, 100 + RESTART_MS, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_FALSE(dup.has_event);
}

TEST_CASE("button fsm exact restart boundary classifies RESTART", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);
    (void)board_button_fsm_feed(&fsm, true, 0, RESTART_MS, FACTORY_MS);

    board_button_fsm_result_t r =
        board_button_fsm_feed(&fsm, false, RESTART_MS, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_TRUE(r.has_event);
    TEST_ASSERT_EQUAL(BOARD_IO_EVENT_RESTART_REQUEST, r.event);
}

TEST_CASE("button fsm restart range classifies RESTART", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);
    (void)board_button_fsm_feed(&fsm, true, 0, RESTART_MS, FACTORY_MS);

    board_button_fsm_result_t r =
        board_button_fsm_feed(&fsm, false, RESTART_MS + 1, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_TRUE(r.has_event);
    TEST_ASSERT_EQUAL(BOARD_IO_EVENT_RESTART_REQUEST, r.event);
}

TEST_CASE("button fsm factory boundary minus one classifies RESTART", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);
    (void)board_button_fsm_feed(&fsm, true, 0, RESTART_MS, FACTORY_MS);

    board_button_fsm_result_t r =
        board_button_fsm_feed(&fsm, true, FACTORY_MS - 1, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_FALSE(r.has_event);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_RESTART_ARMED, r.overlay);

    board_button_fsm_result_t rel =
        board_button_fsm_feed(&fsm, false, FACTORY_MS - 1, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_TRUE(rel.has_event);
    TEST_ASSERT_EQUAL(BOARD_IO_EVENT_RESTART_REQUEST, rel.event);
}

TEST_CASE("button fsm exact factory boundary classifies FACTORY_RESET", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);
    (void)board_button_fsm_feed(&fsm, true, 0, RESTART_MS, FACTORY_MS);

    board_button_fsm_result_t armed =
        board_button_fsm_feed(&fsm, true, FACTORY_MS, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_FACTORY_ARMED, armed.overlay);

    board_button_fsm_result_t rel =
        board_button_fsm_feed(&fsm, false, FACTORY_MS + 50, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_TRUE(rel.has_event);
    TEST_ASSERT_EQUAL(BOARD_IO_EVENT_FACTORY_RESET_REQUEST, rel.event);
}

TEST_CASE("button fsm long factory hold emits exactly one factory on release", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);
    (void)board_button_fsm_feed(&fsm, true, 0, RESTART_MS, FACTORY_MS);

    int factory_events = 0;
    for (uint64_t t = 500; t <= 20000; t += 500) {
        board_button_fsm_result_t r =
            board_button_fsm_feed(&fsm, true, t, RESTART_MS, FACTORY_MS);
        TEST_ASSERT_FALSE(r.has_event);
    }
    board_button_fsm_result_t rel =
        board_button_fsm_feed(&fsm, false, 20500, RESTART_MS, FACTORY_MS);
    if (rel.has_event && rel.event == BOARD_IO_EVENT_FACTORY_RESET_REQUEST) {
        factory_events++;
    }
    TEST_ASSERT_EQUAL_INT(1, factory_events);
    TEST_ASSERT_EQUAL(BOARD_BUTTON_FSM_RELEASED, fsm.state);
}

TEST_CASE("button fsm no event while still held past thresholds", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);
    (void)board_button_fsm_feed(&fsm, true, 0, RESTART_MS, FACTORY_MS);

    for (uint64_t t = 250; t <= 9000; t += 250) {
        board_button_fsm_result_t r =
            board_button_fsm_feed(&fsm, true, t, RESTART_MS, FACTORY_MS);
        TEST_ASSERT_FALSE(r.has_event);
    }
}

TEST_CASE("button fsm factory armed supersedes restart armed", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);
    (void)board_button_fsm_feed(&fsm, true, 0, RESTART_MS, FACTORY_MS);

    board_button_fsm_result_t r1 =
        board_button_fsm_feed(&fsm, true, RESTART_MS, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_RESTART_ARMED, r1.overlay);

    board_button_fsm_result_t r2 =
        board_button_fsm_feed(&fsm, true, FACTORY_MS, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_FACTORY_ARMED, r2.overlay);
}

TEST_CASE("button fsm duplicate released samples emit nothing", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);
    (void)board_button_fsm_feed(&fsm, true, 0, RESTART_MS, FACTORY_MS);

    board_button_fsm_result_t rel =
        board_button_fsm_feed(&fsm, false, 100, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_TRUE(rel.has_event);

    for (int i = 0; i < 5; i++) {
        board_button_fsm_result_t r =
            board_button_fsm_feed(&fsm, false, 200 + (uint64_t)i, RESTART_MS, FACTORY_MS);
        TEST_ASSERT_FALSE(r.has_event);
    }
}

TEST_CASE("button fsm timestamp regression resets without destructive event", "[board_io][fsm]")
{
    board_button_fsm_t fsm;
    fsm_fresh(&fsm);
    (void)board_button_fsm_feed(&fsm, true, 10000, RESTART_MS, FACTORY_MS);

    board_button_fsm_result_t r =
        board_button_fsm_feed(&fsm, true, 9999, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_FALSE(r.has_event);
    TEST_ASSERT_EQUAL(BOARD_BUTTON_FSM_RELEASED, fsm.state);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_NONE, r.overlay);

    board_button_fsm_result_t rel =
        board_button_fsm_feed(&fsm, false, 10500, RESTART_MS, FACTORY_MS);
    TEST_ASSERT_FALSE(rel.has_event);
}
