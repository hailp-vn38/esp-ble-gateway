#ifndef BOARD_BUTTON_FSM_H
#define BOARD_BUTTON_FSM_H

#include <stdbool.h>
#include <stdint.h>

#include "board_io.h"
#include "board_io_internal.h"

typedef enum {
    BOARD_BUTTON_FSM_RELEASED = 0,
    BOARD_BUTTON_FSM_PRESSED,
    BOARD_BUTTON_FSM_RESTART_ARMED,
    BOARD_BUTTON_FSM_FACTORY_ARMED,
} board_button_state_t;

typedef struct {
    board_button_state_t state;
    bool pressed;
    uint64_t press_start_ms;
} board_button_fsm_t;

typedef struct {
    bool has_event;
    board_io_event_t event;
    board_led_overlay_t overlay;
} board_button_fsm_result_t;

void board_button_fsm_init(board_button_fsm_t *fsm);

board_button_fsm_result_t board_button_fsm_feed(
    board_button_fsm_t *fsm,
    bool pressed_now,
    uint64_t now_ms,
    uint32_t restart_ms,
    uint32_t factory_ms
);

#endif
