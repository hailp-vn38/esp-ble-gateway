#include <string.h>

#include "unity.h"

#include "board_led_pattern.h"

static void eval(
    board_status_t base,
    board_led_overlay_t overlay,
    bool identify_active,
    bool activity_active,
    uint64_t epoch_ms,
    uint64_t now_ms,
    board_led_pattern_output_t *out
)
{
    board_led_pattern_input_t in = {
        .base_status = base,
        .overlay = overlay,
        .identify_active = identify_active,
        .activity_active = activity_active,
        .pattern_epoch_ms = epoch_ms,
        .now_ms = now_ms,
    };
    board_led_pattern_evaluate(&in, out);
}

TEST_CASE("led booting pattern boundaries", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_BOOTING, BOARD_LED_OVERLAY_NONE, false, false, 0, 0, &out);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(100, out.rel_next_transition_ms);

    eval(BOARD_STATUS_BOOTING, BOARD_LED_OVERLAY_NONE, false, false, 0, 99, &out);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(1, out.rel_next_transition_ms);

    eval(BOARD_STATUS_BOOTING, BOARD_LED_OVERLAY_NONE, false, false, 0, 100, &out);
    TEST_ASSERT_FALSE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(100, out.rel_next_transition_ms);

    eval(BOARD_STATUS_BOOTING, BOARD_LED_OVERLAY_NONE, false, false, 0, 199, &out);
    TEST_ASSERT_FALSE(out.logical_on);

    eval(BOARD_STATUS_BOOTING, BOARD_LED_OVERLAY_NONE, false, false, 0, 200, &out);
    TEST_ASSERT_TRUE(out.logical_on);
}

TEST_CASE("led provisioning pattern", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_PROVISIONING, BOARD_LED_OVERLAY_NONE, false, false, 0, 0, &out);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(500, out.rel_next_transition_ms);

    eval(BOARD_STATUS_PROVISIONING, BOARD_LED_OVERLAY_NONE, false, false, 0, 499, &out);
    TEST_ASSERT_TRUE(out.logical_on);

    eval(BOARD_STATUS_PROVISIONING, BOARD_LED_OVERLAY_NONE, false, false, 0, 500, &out);
    TEST_ASSERT_FALSE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(500, out.rel_next_transition_ms);
}

TEST_CASE("led wifi connecting pattern", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_WIFI_CONNECTING, BOARD_LED_OVERLAY_NONE, false, false, 0, 0, &out);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(125, out.rel_next_transition_ms);

    eval(BOARD_STATUS_WIFI_CONNECTING, BOARD_LED_OVERLAY_NONE, false, false, 0, 125, &out);
    TEST_ASSERT_FALSE(out.logical_on);

    eval(BOARD_STATUS_WIFI_CONNECTING, BOARD_LED_OVERLAY_NONE, false, false, 0, 250, &out);
    TEST_ASSERT_TRUE(out.logical_on);
}

TEST_CASE("led ready is steady on without transition", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_READY, BOARD_LED_OVERLAY_NONE, false, false, 0, 1234567, &out);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(BOARD_LED_PATTERN_NO_TRANSITION, out.rel_next_transition_ms);
}

TEST_CASE("led degraded full cycle", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_DEGRADED, BOARD_LED_OVERLAY_NONE, false, false, 0, 0, &out);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(150, out.rel_next_transition_ms);

    eval(BOARD_STATUS_DEGRADED, BOARD_LED_OVERLAY_NONE, false, false, 0, 150, &out);
    TEST_ASSERT_FALSE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(850, out.rel_next_transition_ms);

    eval(BOARD_STATUS_DEGRADED, BOARD_LED_OVERLAY_NONE, false, false, 0, 999, &out);
    TEST_ASSERT_FALSE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(1, out.rel_next_transition_ms);

    eval(BOARD_STATUS_DEGRADED, BOARD_LED_OVERLAY_NONE, false, false, 0, 1000, &out);
    TEST_ASSERT_TRUE(out.logical_on);
}

TEST_CASE("led error triple blink plus pause", "[board_io][led]")
{
    board_led_pattern_output_t out;
    const uint64_t checks[][3] = {
        {0, true, 150}, {149, true, 1}, {150, false, 150},
        {300, true, 150}, {450, false, 150},
        {600, true, 150}, {750, false, 1000},
        {1749, false, 1}, {1750, true, 150},
    };

    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        uint64_t t = checks[i][0];
        bool want_on = (checks[i][1] != 0);
        uint64_t want_rel = checks[i][2];
        eval(BOARD_STATUS_ERROR, BOARD_LED_OVERLAY_NONE, false, false, 0, t, &out);
        if (want_on) {
            TEST_ASSERT_TRUE(out.logical_on);
        } else {
            TEST_ASSERT_FALSE(out.logical_on);
        }
        TEST_ASSERT_EQUAL_UINT64(want_rel, out.rel_next_transition_ms);
    }
}

TEST_CASE("led base state change resets pattern phase via epoch", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_BOOTING, BOARD_LED_OVERLAY_NONE, false, false, 0, 150, &out);
    TEST_ASSERT_FALSE(out.logical_on);

    eval(BOARD_STATUS_PROVISIONING, BOARD_LED_OVERLAY_NONE, false, false, 150, 150, &out);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(500, out.rel_next_transition_ms);
}

TEST_CASE("led activity overlay forces steady on", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_READY, BOARD_LED_OVERLAY_ACTIVITY, false, true, 0, 777, &out);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_ACTIVITY, out.effective);

    eval(BOARD_STATUS_READY, BOARD_LED_OVERLAY_ACTIVITY, false, false, 0, 777, &out);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_NONE, out.effective);
}

TEST_CASE("led identify blinks anchored to epoch while active", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_READY, BOARD_LED_OVERLAY_IDENTIFY, true, false, 1000, 1000, &out);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(250, out.rel_next_transition_ms);

    eval(BOARD_STATUS_READY, BOARD_LED_OVERLAY_IDENTIFY, true, false, 1000, 1250, &out);
    TEST_ASSERT_FALSE(out.logical_on);

    eval(BOARD_STATUS_READY, BOARD_LED_OVERLAY_IDENTIFY, true, false, 2000, 2100, &out);
    TEST_ASSERT_TRUE(out.logical_on);
}

TEST_CASE("led latest base wins after overlay expiry", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_WIFI_CONNECTING, BOARD_LED_OVERLAY_IDENTIFY, false, false, 0, 50, &out);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_NONE, out.effective);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(75, out.rel_next_transition_ms);
}

TEST_CASE("led error not obscured by identify or activity", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_ERROR, BOARD_LED_OVERLAY_IDENTIFY, true, false, 0, 10, &out);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_NONE, out.effective);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(140, out.rel_next_transition_ms);

    eval(BOARD_STATUS_ERROR, BOARD_LED_OVERLAY_ACTIVITY, false, true, 0, 10, &out);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_NONE, out.effective);
    TEST_ASSERT_TRUE(out.logical_on);
}

TEST_CASE("led factory armed beats error", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_ERROR, BOARD_LED_OVERLAY_FACTORY_ARMED, false, false, 0, 50, &out);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_FACTORY_ARMED, out.effective);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(50, out.rel_next_transition_ms);

    eval(BOARD_STATUS_ERROR, BOARD_LED_OVERLAY_FACTORY_ARMED, false, false, 0, 150, &out);
    TEST_ASSERT_FALSE(out.logical_on);
}

TEST_CASE("led restart armed visible even when base is error", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_ERROR, BOARD_LED_OVERLAY_RESTART_ARMED, true, true, 0, 9999, &out);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_RESTART_ARMED, out.effective);
    TEST_ASSERT_TRUE(out.logical_on);
    TEST_ASSERT_EQUAL_UINT32(BOARD_LED_PATTERN_NO_TRANSITION, out.rel_next_transition_ms);
}

TEST_CASE("led armed overlays unaffected by transient flags", "[board_io][led]")
{
    board_led_pattern_output_t out;

    eval(BOARD_STATUS_READY, BOARD_LED_OVERLAY_FACTORY_ARMED, true, true, 0, 30, &out);
    TEST_ASSERT_EQUAL(BOARD_LED_OVERLAY_FACTORY_ARMED, out.effective);
    TEST_ASSERT_TRUE(out.logical_on);
}
