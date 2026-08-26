#include "board_display_backend.h"

static esp_err_t none_init(void)
{
    return ESP_OK;
}

static void none_deinit(void)
{
}

static esp_err_t none_set_enabled(bool enabled)
{
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t none_render(const board_display_frame_t *frame)
{
    (void)frame;
    return ESP_ERR_NOT_SUPPORTED;
}

const board_display_backend_t BOARD_DISPLAY_BACKEND_NONE = {
    .init = none_init,
    .deinit = none_deinit,
    .set_enabled = none_set_enabled,
    .render = none_render,
};
