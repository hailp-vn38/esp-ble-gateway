#ifndef BOARD_LED_H
#define BOARD_LED_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "board_io.h"
#include "board_io_internal.h"

esp_err_t board_led_init(int gpio, bool active_low);

void board_led_deinit(void);

bool board_led_is_enabled(void);

void board_led_set_base(board_status_t status, uint64_t now_ms);

void board_led_activity_pulse(uint64_t now_ms);

void board_led_identify_start(uint64_t now_ms);

void board_led_set_armed(board_led_overlay_t overlay, uint64_t now_ms);

void board_led_process(uint64_t now_ms);

bool board_led_next_deadline(uint64_t now_ms, uint64_t *deadline_ms);

#endif
