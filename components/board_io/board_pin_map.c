#include "board_pin_map.h"

#include "esp_log.h"

#include "driver/gpio.h"

static const char *TAG = "board_io";

static bool pin_in(int gpio, int lo, int hi)
{
    return gpio >= lo && gpio <= hi;
}

static bool is_strapping(int gpio)
{
    return gpio == 0 || gpio == 3 || gpio == 45 || gpio == 46;
}

static esp_err_t check_gpio_usable(int gpio, const char *name, bool need_output)
{
    if (gpio < 0) {
        ESP_LOGE(TAG, "%s enabled but GPIO is unset", name);
        return ESP_ERR_INVALID_ARG;
    }
    if (!GPIO_IS_VALID_GPIO(gpio)) {
        ESP_LOGE(TAG, "%s GPIO %d is not valid on this chip", name, gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (need_output && !GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        ESP_LOGE(TAG, "%s GPIO %d cannot be used as output", name, gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (pin_in(gpio, 26, 32)) {
        ESP_LOGE(TAG, "%s GPIO %d is reserved for flash", name, gpio);
        return ESP_ERR_INVALID_ARG;
    }
#ifdef CONFIG_SPIRAM_MODE_OCT
    if (pin_in(gpio, 33, 37)) {
        ESP_LOGE(TAG, "%s GPIO %d is reserved for octal PSRAM", name, gpio);
        return ESP_ERR_INVALID_ARG;
    }
#else
    if (pin_in(gpio, 33, 37)) {
        ESP_LOGW(TAG, "%s GPIO %d collides with pins used by octal PSRAM boards", name, gpio);
    }
#endif
    if (is_strapping(gpio)) {
        ESP_LOGW(TAG, "%s GPIO %d is a strapping pin; schematic review required", name, gpio);
    }
    if (gpio == 43 || gpio == 44) {
        ESP_LOGW(TAG, "%s GPIO %d is the default UART0 console pin", name, gpio);
    }
    return ESP_OK;
}

esp_err_t board_pin_map_validate(const board_pin_map_t *map)
{
    if (map == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (map->button_enabled && map->led_enabled &&
        map->button_gpio == map->led_gpio) {
        ESP_LOGE(TAG, "Button and LED share GPIO %d", map->button_gpio);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc;
    if (map->button_enabled) {
        rc = check_gpio_usable(map->button_gpio, "Button", false);
        if (rc != ESP_OK) {
            return rc;
        }
        if (map->button_active_low && map->button_pull == BOARD_PIN_PULL_DOWN) {
            ESP_LOGE(TAG, "Active-low button with internal pull-down floats into phantom presses");
            return ESP_ERR_INVALID_ARG;
        }
        if (!map->button_active_low && map->button_pull == BOARD_PIN_PULL_UP) {
            ESP_LOGE(TAG, "Active-high button with internal pull-up floats into phantom presses");
            return ESP_ERR_INVALID_ARG;
        }
        if (map->button_pull == BOARD_PIN_PULL_NONE) {
            ESP_LOGW(TAG, "Button has no internal pull; external resistor required");
        }
        if (map->debounce_ms >= map->restart_ms) {
            ESP_LOGE(TAG, "Debounce %u ms must be smaller than restart threshold %u ms",
                     (unsigned)map->debounce_ms, (unsigned)map->restart_ms);
            return ESP_ERR_INVALID_ARG;
        }
        if (map->factory_reset_ms <= map->restart_ms) {
            ESP_LOGE(TAG, "Factory threshold %u ms must exceed restart threshold %u ms",
                     (unsigned)map->factory_reset_ms, (unsigned)map->restart_ms);
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (map->led_enabled) {
        rc = check_gpio_usable(map->led_gpio, "LED", true);
        if (rc != ESP_OK) {
            return rc;
        }
    }

    return ESP_OK;
}
