#include "board_display.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if CONFIG_BOARD_IO_DISPLAY_ENABLE
#define DISPLAY_CAP 1
#else
#define DISPLAY_CAP 0
#endif

#if DISPLAY_CAP
static const char *TAG = "board_io";
static SemaphoreHandle_t s_lock;
static bool s_runtime_enabled;
static bool s_dirty;
static uint64_t s_next_allowed;
static board_display_frame_t s_pending;
static board_display_frame_t s_snapshot;
static const board_display_backend_t *s_backend;

static int64_t s_last_error_log_us;
#endif

bool board_display_capability_enabled(void)
{
    return DISPLAY_CAP == 1;
}

#if DISPLAY_CAP
static uint32_t refresh_period_ms(void)
{
    uint32_t hz = (uint32_t)CONFIG_BOARD_IO_DISPLAY_MAX_REFRESH_HZ;
    if (hz == 0) {
        hz = 1;
    }
    return 1000U / hz;
}

static const board_display_backend_t *configured_backend(void)
{
    return NULL;
}
#endif

esp_err_t board_display_init(void)
{
#if !DISPLAY_CAP
    return ESP_OK;
#else
    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_pending, 0, sizeof(s_pending));
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_dirty = false;
    s_next_allowed = 0;
    s_runtime_enabled = true;
    s_last_error_log_us = 0;

    const board_display_backend_t *backend = configured_backend();
    if (backend != NULL) {
        esp_err_t rc = backend->init();
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "Display backend init failed: %s", esp_err_to_name(rc));
#if defined(CONFIG_BOARD_IO_DISPLAY_REQUIRED)
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
            return rc;
#else
            backend = NULL;
#endif
        }
    }
    s_backend = backend;
    return ESP_OK;
#endif
}

void board_display_deinit(void)
{
#if DISPLAY_CAP
    if (s_lock == NULL) {
        return;
    }
    if (s_backend != NULL) {
        s_backend->deinit();
    }
    s_backend = NULL;
    s_runtime_enabled = false;
    s_dirty = false;
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
#endif
}

esp_err_t board_display_update(const board_display_frame_t *frame)
{
#if !DISPLAY_CAP
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_pending = *frame;
    for (size_t i = 0; i < BOARD_IO_DISPLAY_LINES; i++) {
        s_pending.line[i][BOARD_IO_DISPLAY_LINE_LEN - 1] = '\0';
    }
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
#endif
}

esp_err_t board_display_set_runtime_enabled(bool enabled)
{
#if !DISPLAY_CAP
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enabled && s_backend == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_runtime_enabled = enabled;
    xSemaphoreGive(s_lock);
    return ESP_OK;
#endif
}

bool board_display_wants_render(uint64_t now_ms, uint64_t *next_allowed_ms)
{
#if !DISPLAY_CAP
    (void)now_ms;
    (void)next_allowed_ms;
    return false;
#else
    if (s_lock == NULL || !s_dirty || !s_runtime_enabled || s_backend == NULL) {
        return false;
    }
    *next_allowed_ms = (now_ms < s_next_allowed) ? s_next_allowed : now_ms;
    return true;
#endif
}

void board_display_process(uint64_t now_ms)
{
#if DISPLAY_CAP
    if (s_lock == NULL || !s_dirty || !s_runtime_enabled || s_backend == NULL) {
        return;
    }
    if (now_ms < s_next_allowed) {
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    s_snapshot = s_pending;
    s_dirty = false;
    xSemaphoreGive(s_lock);

    s_next_allowed = board_deadline_add_ms(now_ms, refresh_period_ms());

    esp_err_t rc = s_backend->render(&s_snapshot);
    if (rc != ESP_OK) {
        int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_error_log_us >= 1000000LL) {
            ESP_LOGW(TAG, "Display render failed: %s", esp_err_to_name(rc));
            s_last_error_log_us = now_us;
        }
    }
#endif
}

const board_display_backend_t *board_display_active_backend(void)
{
#if !DISPLAY_CAP
    return NULL;
#else
    return s_backend;
#endif
}

void board_display_test_set_backend(const board_display_backend_t *backend)
{
#if DISPLAY_CAP
    s_backend = backend;
#else
    (void)backend;
#endif
}

void board_display_test_reset(void)
{
#if DISPLAY_CAP
    if (s_lock != NULL) {
        memset(&s_pending, 0, sizeof(s_pending));
        memset(&s_snapshot, 0, sizeof(s_snapshot));
        s_dirty = false;
        s_next_allowed = 0;
        s_runtime_enabled = true;
        s_last_error_log_us = 0;
        s_backend = configured_backend();
    }
#endif
}
