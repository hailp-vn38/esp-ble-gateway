#ifndef BOARD_IO_INTERNAL_H
#define BOARD_IO_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_io.h"

typedef enum {
    BOARD_IO_LC_UNINITIALIZED = 0,
    BOARD_IO_LC_INITIALIZING,
    BOARD_IO_LC_RUNNING,
    BOARD_IO_LC_STOPPING,
} board_io_lc_t;

#define BOARD_NOTIFY_BUTTON_EDGE     (1UL << 0)
#define BOARD_NOTIFY_STATUS_CHANGED  (1UL << 1)
#define BOARD_NOTIFY_ACTIVITY        (1UL << 2)
#define BOARD_NOTIFY_IDENTIFY        (1UL << 3)
#define BOARD_NOTIFY_DISPLAY         (1UL << 4)
#define BOARD_NOTIFY_STOP            (1UL << 5)

#define BOARD_IO_WAIT_NONE UINT32_MAX

typedef enum {
    BOARD_LED_OVERLAY_NONE = 0,
    BOARD_LED_OVERLAY_ACTIVITY,
    BOARD_LED_OVERLAY_IDENTIFY,
    BOARD_LED_OVERLAY_RESTART_ARMED,
    BOARD_LED_OVERLAY_FACTORY_ARMED,
} board_led_overlay_t;

uint64_t board_time_now_ms(void);

uint64_t board_deadline_add_ms(uint64_t now_ms, uint32_t delta_ms);

#endif
