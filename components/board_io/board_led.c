#include "board_led.h"

#include "esp_log.h"

#include "driver/gpio.h"

#include "board_led_pattern.h"

static const char *TAG = "board_io";

#define ACTIVITY_PULSE_MS 80U
#define IDENTIFY_DURATION_MS 5000U

static bool s_enabled;
static int s_gpio = -1;
static bool s_active_low;

static board_status_t s_base = BOARD_STATUS_BOOTING;
static board_led_overlay_t s_overlay = BOARD_LED_OVERLAY_NONE;
static uint64_t s_base_epoch;
static uint64_t s_overlay_epoch;

static uint64_t s_activity_deadline;
static uint64_t s_identify_deadline;

static bool s_last_on;
static bool s_has_level;

static uint64_t pick_anchor(board_led_overlay_t effective)
{
    switch (effective) {
    case BOARD_LED_OVERLAY_IDENTIFY:
    case BOARD_LED_OVERLAY_RESTART_ARMED:
    case BOARD_LED_OVERLAY_FACTORY_ARMED:
        return s_overlay_epoch;
    default:
        return s_base_epoch;
    }
}

static void drive(bool logical_on)
{
    if (s_gpio >= 0 && (!s_has_level || logical_on != s_last_on)) {
        gpio_set_level((gpio_num_t)s_gpio, s_active_low ? (logical_on ? 0 : 1)
                                                        : (logical_on ? 1 : 0));
        s_last_on = logical_on;
        s_has_level = true;
    }
}

esp_err_t board_led_init(int gpio, bool active_low)
{
    if (gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t rc = gpio_config(&io);
    if (rc != ESP_OK) {
        return rc;
    }

    s_enabled = true;
    s_gpio = gpio;
    s_active_low = active_low;
    s_base = BOARD_STATUS_BOOTING;
    s_overlay = BOARD_LED_OVERLAY_NONE;
    s_base_epoch = board_time_now_ms();
    s_overlay_epoch = s_base_epoch;
    s_activity_deadline = 0;
    s_identify_deadline = 0;
    s_has_level = false;

    drive(false);
    return ESP_OK;
}

void board_led_deinit(void)
{
    if (s_enabled) {
        drive(false);
    }
    s_enabled = false;
    s_gpio = -1;
    s_overlay = BOARD_LED_OVERLAY_NONE;
    s_activity_deadline = 0;
    s_identify_deadline = 0;
    s_has_level = false;
}

bool board_led_is_enabled(void)
{
    return s_enabled;
}

void board_led_set_base(board_status_t status, uint64_t now_ms)
{
    if (!s_enabled) {
        return;
    }
    if (status != s_base) {
        s_base = status;
        s_base_epoch = now_ms;
    }
}

void board_led_activity_pulse(uint64_t now_ms)
{
    if (!s_enabled) {
        return;
    }
    s_overlay = BOARD_LED_OVERLAY_ACTIVITY;
    s_activity_deadline = board_deadline_add_ms(now_ms, ACTIVITY_PULSE_MS);
}

void board_led_identify_start(uint64_t now_ms)
{
    if (!s_enabled) {
        return;
    }
    s_overlay = BOARD_LED_OVERLAY_IDENTIFY;
    s_identify_deadline = board_deadline_add_ms(now_ms, IDENTIFY_DURATION_MS);
    s_overlay_epoch = now_ms;
}

void board_led_set_armed(board_led_overlay_t overlay, uint64_t now_ms)
{
    if (!s_enabled) {
        return;
    }
    if (overlay == BOARD_LED_OVERLAY_NONE) {
        if (s_overlay == BOARD_LED_OVERLAY_RESTART_ARMED ||
            s_overlay == BOARD_LED_OVERLAY_FACTORY_ARMED) {
            s_overlay = BOARD_LED_OVERLAY_NONE;
        }
        return;
    }
    if (overlay != s_overlay) {
        s_overlay = overlay;
        s_overlay_epoch = now_ms;
    }
}

void board_led_process(uint64_t now_ms)
{
    if (!s_enabled) {
        return;
    }

    if (s_overlay == BOARD_LED_OVERLAY_ACTIVITY && now_ms >= s_activity_deadline) {
        s_overlay = BOARD_LED_OVERLAY_NONE;
    }
    if (s_overlay == BOARD_LED_OVERLAY_IDENTIFY && now_ms >= s_identify_deadline) {
        s_overlay = BOARD_LED_OVERLAY_NONE;
    }

    board_led_pattern_input_t in = {
        .base_status = s_base,
        .overlay = s_overlay,
        .identify_active = now_ms < s_identify_deadline,
        .activity_active = now_ms < s_activity_deadline,
        .pattern_epoch_ms = pick_anchor(s_overlay),
        .now_ms = now_ms,
    };
    board_led_pattern_output_t out;
    board_led_pattern_evaluate(&in, &out);
    drive(out.logical_on);
}

bool board_led_next_deadline(uint64_t now_ms, uint64_t *deadline_ms)
{
    if (!s_enabled) {
        return false;
    }

    uint64_t best = UINT64_MAX;
    uint64_t candidate;

    if (s_overlay == BOARD_LED_OVERLAY_ACTIVITY) {
        best = s_activity_deadline;
    } else if (s_overlay == BOARD_LED_OVERLAY_IDENTIFY) {
        best = s_identify_deadline;
    }

    board_led_pattern_input_t probe = {
        .base_status = s_base,
        .overlay = s_overlay,
        .identify_active = now_ms < s_identify_deadline,
        .activity_active = now_ms < s_activity_deadline,
        .pattern_epoch_ms = pick_anchor(s_overlay),
        .now_ms = now_ms,
    };
    board_led_pattern_output_t out;
    board_led_pattern_evaluate(&probe, &out);
    if (out.rel_next_transition_ms != BOARD_LED_PATTERN_NO_TRANSITION) {
        candidate = board_deadline_add_ms(now_ms, out.rel_next_transition_ms);
        if (candidate < best) {
            best = candidate;
        }
    }

    if (best != UINT64_MAX) {
        *deadline_ms = best;
        return true;
    }
    return false;
}
