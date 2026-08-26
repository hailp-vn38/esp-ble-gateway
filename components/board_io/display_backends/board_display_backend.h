#ifndef BOARD_DISPLAY_BACKEND_H
#define BOARD_DISPLAY_BACKEND_H

#include "esp_err.h"

#include "board_io.h"

typedef struct {
    esp_err_t (*init)(void);
    void (*deinit)(void);
    esp_err_t (*set_enabled)(bool enabled);
    esp_err_t (*render)(const board_display_frame_t *frame);
} board_display_backend_t;

extern const board_display_backend_t BOARD_DISPLAY_BACKEND_NONE;

#endif
