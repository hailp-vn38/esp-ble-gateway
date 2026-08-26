set(board_io_priv_reqs
    esp_driver_gpio
    esp_timer
    log
)

if(CONFIG_BOARD_IO_DISPLAY_BACKEND_SSD1306_I2C)
    list(APPEND board_io_priv_reqs esp_driver_i2c)
endif()

idf_component_register(
    SRCS
        "board_io.c"
        "board_pin_map.c"
        "board_button.c"
        "board_button_fsm.c"
        "board_led.c"
        "board_led_pattern.c"
        "board_display.c"
        "display_backends/board_display_none.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
        "display_backends"
    PRIV_REQUIRES
        ${board_io_priv_reqs}
)
