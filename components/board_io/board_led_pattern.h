#ifndef BOARD_LED_PATTERN_H
#define BOARD_LED_PATTERN_H

#include <stdbool.h>
#include <stdint.h>

#include "board_io.h"
#include "board_io_internal.h"

#define BOARD_LED_PATTERN_NO_TRANSITION UINT32_MAX

typedef struct {
    board_status_t base_status;
    board_led_overlay_t overlay;
    bool identify_active;
    bool activity_active;
    uint64_t pattern_epoch_ms;
    uint64_t now_ms;
} board_led_pattern_input_t;

typedef struct {
    board_led_overlay_t effective;
    bool logical_on;
    uint32_t rel_next_transition_ms;
} board_led_pattern_output_t;

void board_led_pattern_evaluate(
    const board_led_pattern_input_t *input,
    board_led_pattern_output_t *output
);

#endif
