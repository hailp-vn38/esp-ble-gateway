#include "unity.h"

#include "board_pin_map.h"

static const uint32_t DEBOUNCE_MS = 40U;
static const uint32_t RESTART_MS = 2000U;
static const uint32_t FACTORY_MS = 8000U;

static board_pin_map_t base_map(void)
{
    board_pin_map_t m;
    m.button_enabled = false;
    m.button_gpio = -1;
    m.button_active_low = true;
    m.button_pull = BOARD_PIN_PULL_UP;
    m.debounce_ms = DEBOUNCE_MS;
    m.restart_ms = RESTART_MS;
    m.factory_reset_ms = FACTORY_MS;
    m.led_enabled = false;
    m.led_gpio = -1;
    return m;
}

TEST_CASE("pin map rejects shared button and led gpio", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.button_enabled = true;
    m.button_gpio = 5;
    m.led_enabled = true;
    m.led_gpio = 5;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&m));
}

TEST_CASE("pin map rejects unset gpio on enabled feature", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.button_enabled = true;
    m.button_gpio = -1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&m));

    board_pin_map_t l = base_map();
    l.led_enabled = true;
    l.led_gpio = -1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&l));
}

TEST_CASE("pin map rejects chip-invalid gpio 22", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.button_enabled = true;
    m.button_gpio = 22;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&m));
}

TEST_CASE("pin map rejects chip-invalid gpio 23", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.led_enabled = true;
    m.led_gpio = 23;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&m));
}

TEST_CASE("pin map rejects chip-invalid gpio 24", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.button_enabled = true;
    m.button_gpio = 24;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&m));
}

TEST_CASE("pin map rejects chip-invalid gpio 25", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.led_enabled = true;
    m.led_gpio = 25;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&m));
}

TEST_CASE("pin map rejects flash blocklist gpio 26", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.button_enabled = true;
    m.button_gpio = 26;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&m));
}

TEST_CASE("pin map rejects flash blocklist gpio 31", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.led_enabled = true;
    m.led_gpio = 31;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&m));
}

TEST_CASE("pin map applies psram policy on gpio 35", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.led_enabled = true;
    m.led_gpio = 35;
#if defined(CONFIG_SPIRAM_MODE_OCT)
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&m));
#else
    TEST_ASSERT_EQUAL(ESP_OK, board_pin_map_validate(&m));
#endif
}

TEST_CASE("pin map ignores pins of disabled features", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.button_gpio = 27;
    m.led_gpio = 28;
    m.button_enabled = false;
    m.led_enabled = false;
    TEST_ASSERT_EQUAL(ESP_OK, board_pin_map_validate(&m));

    board_pin_map_t partial = base_map();
    partial.button_enabled = true;
    partial.button_gpio = 6;
    partial.led_gpio = 6;
    partial.led_enabled = false;
    TEST_ASSERT_EQUAL(ESP_OK, board_pin_map_validate(&partial));
}

TEST_CASE("pin map rejects factory threshold not above restart", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.button_enabled = true;
    m.button_gpio = 5;
    m.factory_reset_ms = RESTART_MS;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&m));

    board_pin_map_t below = base_map();
    below.button_enabled = true;
    below.button_gpio = 5;
    below.factory_reset_ms = RESTART_MS - 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&below));
}

TEST_CASE("pin map rejects debounce not below restart", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.button_enabled = true;
    m.button_gpio = 5;
    m.debounce_ms = RESTART_MS;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&m));

    board_pin_map_t above = base_map();
    above.button_enabled = true;
    above.button_gpio = 5;
    above.debounce_ms = RESTART_MS + 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&above));
}

TEST_CASE("pin map accepts distinct enabled pins", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.button_enabled = true;
    m.button_gpio = 4;
    m.led_enabled = true;
    m.led_gpio = 5;
    TEST_ASSERT_EQUAL(ESP_OK, board_pin_map_validate(&m));
}

TEST_CASE("pin map rejects polarity pull contradiction", "[board_io][pin]")
{
    board_pin_map_t low_down = base_map();
    low_down.button_enabled = true;
    low_down.button_gpio = 4;
    low_down.button_active_low = true;
    low_down.button_pull = BOARD_PIN_PULL_DOWN;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&low_down));

    board_pin_map_t high_up = base_map();
    high_up.button_enabled = true;
    high_up.button_gpio = 4;
    high_up.button_active_low = false;
    high_up.button_pull = BOARD_PIN_PULL_UP;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_pin_map_validate(&high_up));
}

TEST_CASE("pin map allows external pull mode with warning", "[board_io][pin]")
{
    board_pin_map_t m = base_map();
    m.button_enabled = true;
    m.button_gpio = 4;
    m.button_pull = BOARD_PIN_PULL_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, board_pin_map_validate(&m));
}
