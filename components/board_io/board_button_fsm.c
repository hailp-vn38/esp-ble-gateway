#include "board_button_fsm.h"

static board_button_fsm_result_t make_result(
    bool has_event,
    board_io_event_t event,
    board_led_overlay_t overlay
)
{
    board_button_fsm_result_t r;
    r.has_event = has_event;
    r.event = event;
    r.overlay = overlay;
    return r;
}

void board_button_fsm_init(board_button_fsm_t *fsm)
{
    fsm->state = BOARD_BUTTON_FSM_RELEASED;
    fsm->pressed = false;
    fsm->press_start_ms = 0;
}

board_button_fsm_result_t board_button_fsm_feed(
    board_button_fsm_t *fsm,
    bool pressed_now,
    uint64_t now_ms,
    uint32_t restart_ms,
    uint32_t factory_ms
)
{
    if (!pressed_now) {
        if (!fsm->pressed) {
            return make_result(false, BOARD_IO_EVENT_COUNT, BOARD_LED_OVERLAY_NONE);
        }
        uint64_t duration = now_ms - fsm->press_start_ms;
        board_io_event_t event;
        if (duration >= factory_ms) {
            event = BOARD_IO_EVENT_FACTORY_RESET_REQUEST;
        } else if (duration >= restart_ms) {
            event = BOARD_IO_EVENT_RESTART_REQUEST;
        } else {
            event = BOARD_IO_EVENT_BUTTON_SHORT_PRESS;
        }
        fsm->state = BOARD_BUTTON_FSM_RELEASED;
        fsm->pressed = false;
        return make_result(true, event, BOARD_LED_OVERLAY_NONE);
    }

    if (!fsm->pressed) {
        fsm->pressed = true;
        fsm->state = BOARD_BUTTON_FSM_PRESSED;
        fsm->press_start_ms = now_ms;
        return make_result(false, BOARD_IO_EVENT_COUNT, BOARD_LED_OVERLAY_NONE);
    }

    if (now_ms < fsm->press_start_ms) {
        fsm->state = BOARD_BUTTON_FSM_RELEASED;
        fsm->pressed = false;
        return make_result(false, BOARD_IO_EVENT_COUNT, BOARD_LED_OVERLAY_NONE);
    }

    uint64_t duration = now_ms - fsm->press_start_ms;
    switch (fsm->state) {
    case BOARD_BUTTON_FSM_PRESSED:
        if (duration >= factory_ms) {
            fsm->state = BOARD_BUTTON_FSM_FACTORY_ARMED;
            return make_result(false, BOARD_IO_EVENT_COUNT, BOARD_LED_OVERLAY_FACTORY_ARMED);
        }
        if (duration >= restart_ms) {
            fsm->state = BOARD_BUTTON_FSM_RESTART_ARMED;
            return make_result(false, BOARD_IO_EVENT_COUNT, BOARD_LED_OVERLAY_RESTART_ARMED);
        }
        break;
    case BOARD_BUTTON_FSM_RESTART_ARMED:
        if (duration >= factory_ms) {
            fsm->state = BOARD_BUTTON_FSM_FACTORY_ARMED;
            return make_result(false, BOARD_IO_EVENT_COUNT, BOARD_LED_OVERLAY_FACTORY_ARMED);
        }
        break;
    default:
        break;
    }

    board_led_overlay_t current = BOARD_LED_OVERLAY_NONE;
    if (fsm->state == BOARD_BUTTON_FSM_RESTART_ARMED) {
        current = BOARD_LED_OVERLAY_RESTART_ARMED;
    } else if (fsm->state == BOARD_BUTTON_FSM_FACTORY_ARMED) {
        current = BOARD_LED_OVERLAY_FACTORY_ARMED;
    }
    return make_result(false, BOARD_IO_EVENT_COUNT, current);
}
