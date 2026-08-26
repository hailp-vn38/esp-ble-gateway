#ifndef BOARD_DISPLAY_H
#define BOARD_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "board_io.h"
#include "board_io_internal.h"
#include "board_display_backend.h"

bool board_display_capability_enabled(void);

esp_err_t board_display_init(void);

void board_display_deinit(void);

esp_err_t board_display_update(const board_display_frame_t *frame);

esp_err_t board_display_set_runtime_enabled(bool enabled);

bool board_display_wants_render(uint64_t now_ms, uint64_t *next_allowed_ms);

void board_display_process(uint64_t now_ms);

const board_display_backend_t *board_display_active_backend(void);

void board_display_test_set_backend(const board_display_backend_t *backend);

void board_display_test_reset(void);

#endif
