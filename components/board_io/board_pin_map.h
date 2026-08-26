#ifndef BOARD_PIN_MAP_H
#define BOARD_PIN_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    BOARD_PIN_PULL_NONE = 0,
    BOARD_PIN_PULL_UP,
    BOARD_PIN_PULL_DOWN,
} board_pin_pull_t;

typedef struct {
    bool button_enabled;
    int button_gpio;
    bool button_active_low;
    board_pin_pull_t button_pull;
    uint32_t debounce_ms;
    uint32_t restart_ms;
    uint32_t factory_reset_ms;

    bool led_enabled;
    int led_gpio;
} board_pin_map_t;

esp_err_t board_pin_map_validate(const board_pin_map_t *map);

#endif
