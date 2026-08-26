#ifndef BOARD_BUTTON_H
#define BOARD_BUTTON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "board_io.h"
#include "board_io_internal.h"
#include "board_pin_map.h"

typedef int (*board_button_level_reader_t)(int gpio);

typedef struct {
    int gpio;
    bool active_low;
    board_pin_pull_t pull;
    uint32_t debounce_ms;
    uint32_t restart_ms;
    uint32_t factory_ms;
} board_button_config_t;

esp_err_t board_button_init(
    const board_button_config_t *cfg,
    TaskHandle_t worker_task
);

void board_button_deinit(void);

bool board_button_is_enabled(void);

void board_button_on_edge_notify(void);

size_t board_button_process(
    uint64_t now_ms,
    board_button_level_reader_t reader,
    board_io_event_t *out_events,
    size_t max_events
);

bool board_button_next_deadline(uint64_t now_ms, uint64_t *deadline_ms);

int board_button_gpio_reader(int gpio);

#endif
