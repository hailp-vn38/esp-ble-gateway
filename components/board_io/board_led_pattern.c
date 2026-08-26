#include "board_led_pattern.h"

#define IDENTIFY_PERIOD_MS   500U
#define IDENTIFY_ON_MS       250U
#define FACTORY_PERIOD_MS    200U
#define FACTORY_ON_MS        100U

#define BOOTING_PERIOD_MS    200U
#define BOOTING_ON_MS        100U

#define PROV_PERIOD_MS       1000U
#define PROV_ON_MS           500U

#define WIFI_CONNECT_PERIOD  250U
#define WIFI_CONNECT_ON      125U

#define DEGRADED_PERIOD_MS   1000U
#define DEGRADED_ON_MS       150U

#define ERROR_CYCLE_MS       1750U

static uint32_t blink_rel(uint64_t elapsed, uint32_t period, uint32_t on_ms, bool *on)
{
    uint32_t phase = (uint32_t)(elapsed % period);
    *on = phase < on_ms;
    if (*on) {
        return on_ms - phase;
    }
    return period - phase;
}

static void evaluate_base(board_status_t status, uint64_t elapsed, bool *on, uint32_t *rel)
{
    switch (status) {
    case BOARD_STATUS_BOOTING:
        *rel = blink_rel(elapsed, BOOTING_PERIOD_MS, BOOTING_ON_MS, on);
        break;
    case BOARD_STATUS_PROVISIONING:
        *rel = blink_rel(elapsed, PROV_PERIOD_MS, PROV_ON_MS, on);
        break;
    case BOARD_STATUS_WIFI_CONNECTING:
        *rel = blink_rel(elapsed, WIFI_CONNECT_PERIOD, WIFI_CONNECT_ON, on);
        break;
    case BOARD_STATUS_READY:
        *on = true;
        *rel = BOARD_LED_PATTERN_NO_TRANSITION;
        break;
    case BOARD_STATUS_DEGRADED:
        *rel = blink_rel(elapsed, DEGRADED_PERIOD_MS, DEGRADED_ON_MS, on);
        break;
    case BOARD_STATUS_ERROR: {
        uint32_t p = (uint32_t)(elapsed % ERROR_CYCLE_MS);
        static const uint32_t bounds[6] = {150, 300, 450, 600, 750, ERROR_CYCLE_MS};
        *on = (p < 150) || (p >= 300 && p < 450) || (p >= 600 && p < 750);
        for (int i = 0; i < 6; i++) {
            if (p < bounds[i]) {
                *rel = bounds[i] - p;
                return;
            }
        }
        *on = false;
        *rel = BOARD_LED_PATTERN_NO_TRANSITION;
        break;
    }
    default:
        *on = false;
        *rel = BOARD_LED_PATTERN_NO_TRANSITION;
        break;
    }
}

void board_led_pattern_evaluate(
    const board_led_pattern_input_t *input,
    board_led_pattern_output_t *output
)
{
    output->logical_on = false;
    output->rel_next_transition_ms = BOARD_LED_PATTERN_NO_TRANSITION;
    output->effective = input->overlay;

    if (input->overlay == BOARD_LED_OVERLAY_FACTORY_ARMED) {
        output->effective = BOARD_LED_OVERLAY_FACTORY_ARMED;
        output->rel_next_transition_ms =
            blink_rel(input->now_ms - input->pattern_epoch_ms,
                      FACTORY_PERIOD_MS, FACTORY_ON_MS,
                      &output->logical_on);
        return;
    }

    if (input->overlay == BOARD_LED_OVERLAY_RESTART_ARMED) {
        output->effective = BOARD_LED_OVERLAY_RESTART_ARMED;
        output->logical_on = true;
        return;
    }

    if (input->base_status == BOARD_STATUS_ERROR) {
        output->effective = BOARD_LED_OVERLAY_NONE;
        evaluate_base(input->base_status,
                      input->now_ms - input->pattern_epoch_ms,
                      &output->logical_on,
                      &output->rel_next_transition_ms);
        return;
    }

    if (input->overlay == BOARD_LED_OVERLAY_IDENTIFY && input->identify_active) {
        output->effective = BOARD_LED_OVERLAY_IDENTIFY;
        output->rel_next_transition_ms =
            blink_rel(input->now_ms - input->pattern_epoch_ms,
                      IDENTIFY_PERIOD_MS, IDENTIFY_ON_MS,
                      &output->logical_on);
        return;
    }

    if (input->overlay == BOARD_LED_OVERLAY_ACTIVITY && input->activity_active) {
        output->effective = BOARD_LED_OVERLAY_ACTIVITY;
        output->logical_on = true;
        return;
    }

    output->effective = BOARD_LED_OVERLAY_NONE;
    evaluate_base(input->base_status,
                  input->now_ms - input->pattern_epoch_ms,
                  &output->logical_on,
                  &output->rel_next_transition_ms);
}
