#ifndef BOARD_IO_H
#define BOARD_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_IO_DISPLAY_LINES     4
#define BOARD_IO_DISPLAY_LINE_LEN  32

typedef enum {
    BOARD_STATUS_BOOTING = 0,
    BOARD_STATUS_PROVISIONING,
    BOARD_STATUS_WIFI_CONNECTING,
    BOARD_STATUS_READY,
    BOARD_STATUS_DEGRADED,
    BOARD_STATUS_ERROR,
    BOARD_STATUS_COUNT,
} board_status_t;

typedef enum {
    BOARD_IO_EVENT_BUTTON_SHORT_PRESS = 0,
    BOARD_IO_EVENT_RESTART_REQUEST,
    BOARD_IO_EVENT_FACTORY_RESET_REQUEST,
    BOARD_IO_EVENT_COUNT,
} board_io_event_t;

typedef enum {
    BOARD_SIGNAL_ACTIVITY = 0,
    BOARD_SIGNAL_IDENTIFY,
    BOARD_SIGNAL_COUNT,
} board_signal_t;

typedef struct {
    char line[BOARD_IO_DISPLAY_LINES][BOARD_IO_DISPLAY_LINE_LEN];
} board_display_frame_t;

typedef void (*board_io_event_handler_t)(
    board_io_event_t event,
    void *context
);

esp_err_t board_io_init(void);
esp_err_t board_io_deinit(void);

bool board_io_is_initialized(void);

esp_err_t board_io_register_event_handler(
    board_io_event_handler_t handler,
    void *context
);

esp_err_t board_io_set_status(board_status_t status);

esp_err_t board_io_signal(board_signal_t signal);

esp_err_t board_io_display_update(
    const board_display_frame_t *frame
);

esp_err_t board_io_display_set_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
