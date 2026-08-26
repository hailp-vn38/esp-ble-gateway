#include "board_io_internal.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_button.h"
#include "board_display.h"
#include "board_led.h"
#include "board_pin_map.h"

static const char *TAG = "board_io";

#define STOP_HANDSHAKE_TIMEOUT_MS 3000U

static SemaphoreHandle_t s_lock;
static board_io_lc_t s_state = BOARD_IO_LC_UNINITIALIZED;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_stopped;
static board_status_t s_desired_status = BOARD_STATUS_BOOTING;
static board_io_event_handler_t s_handler;
static void *s_handler_ctx;

uint64_t board_time_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

uint64_t board_deadline_add_ms(uint64_t now_ms, uint32_t delta_ms)
{
    if (delta_ms >= UINT64_MAX - now_ms) {
        return UINT64_MAX;
    }
    return now_ms + delta_ms;
}

static bool lc_lock_take(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE;
}

static void notify_worker(uint32_t bits)
{
    TaskHandle_t task = s_task;
    if (task != NULL) {
        xTaskNotify(task, bits, eSetBits);
    }
}

#if CONFIG_BOARD_IO_BUTTON_ENABLE
#define BUTTON_CAP 1
#else
#define BUTTON_CAP 0
#endif

#if CONFIG_BOARD_IO_LED_ENABLE
#define LED_CAP 1
#else
#define LED_CAP 0
#endif

static esp_err_t build_and_validate_pin_map(board_pin_map_t *map)
{
    memset(map, 0, sizeof(*map));
    map->button_gpio = -1;
    map->led_gpio = -1;

#if BUTTON_CAP
    map->button_enabled = true;
    map->button_gpio = CONFIG_BOARD_IO_BUTTON_GPIO;
#if CONFIG_BOARD_IO_BUTTON_ACTIVE_LOW
    map->button_active_low = true;
#endif
#if defined(CONFIG_BOARD_IO_BUTTON_PULL_UP)
    map->button_pull = BOARD_PIN_PULL_UP;
#elif defined(CONFIG_BOARD_IO_BUTTON_PULL_DOWN)
    map->button_pull = BOARD_PIN_PULL_DOWN;
#else
    map->button_pull = BOARD_PIN_PULL_NONE;
#endif
    map->debounce_ms = (uint32_t)CONFIG_BOARD_IO_BUTTON_DEBOUNCE_MS;
    map->restart_ms = (uint32_t)CONFIG_BOARD_IO_BUTTON_RESTART_MS;
    map->factory_reset_ms = (uint32_t)CONFIG_BOARD_IO_BUTTON_FACTORY_RESET_MS;
#endif

#if LED_CAP
    map->led_enabled = true;
    map->led_gpio = CONFIG_BOARD_IO_LED_GPIO;
#endif

    return board_pin_map_validate(map);
}

static void worker_loop(void *arg)
{
    (void)arg;

    for (;;) {
        uint64_t now = board_time_now_ms();
        uint64_t best = UINT64_MAX;
        uint64_t candidate;

        if (board_button_next_deadline(now, &candidate) && candidate < best) {
            best = candidate;
        }
        if (board_led_next_deadline(now, &candidate) && candidate < best) {
            best = candidate;
        }
        if (board_display_wants_render(now, &candidate) && candidate < best) {
            best = candidate;
        }

        uint32_t timeout_ms = BOARD_IO_WAIT_NONE;
        if (best != UINT64_MAX) {
            timeout_ms = (best > now) ? (uint32_t)(best - now) : 0U;
        }

        uint32_t bits = 0;
        xTaskNotifyWait(
            0,
            UINT32_MAX,
            &bits,
            (timeout_ms == BOARD_IO_WAIT_NONE)
                ? portMAX_DELAY
                : pdMS_TO_TICKS(timeout_ms));

        now = board_time_now_ms();

        if (bits & BOARD_NOTIFY_STOP) {
            break;
        }

        board_io_event_t event;
        size_t count =
            board_button_process(now, board_button_gpio_reader, &event, 1);
        if (count > 0) {
            board_io_event_handler_t handler = NULL;
            void *ctx = NULL;
            if (lc_lock_take()) {
                handler = s_handler;
                ctx = s_handler_ctx;
                xSemaphoreGive(s_lock);
            }
            if (handler != NULL) {
                handler(event, ctx);
            }
        }

        if (bits & BOARD_NOTIFY_STATUS_CHANGED) {
            board_status_t desired = BOARD_STATUS_BOOTING;
            if (lc_lock_take()) {
                desired = s_desired_status;
                xSemaphoreGive(s_lock);
                board_led_set_base(desired, now);
            }
        }
        if (bits & BOARD_NOTIFY_ACTIVITY) {
            board_led_activity_pulse(now);
        }
        if (bits & BOARD_NOTIFY_IDENTIFY) {
            board_led_identify_start(now);
        }

        board_led_process(now);
        board_display_process(now);
    }

    xSemaphoreGive(s_stopped);
    vTaskDelete(NULL);
}

esp_err_t board_io_init(void)
{
    esp_err_t rc = ESP_OK;
    board_pin_map_t map;
    bool led_on = false;
    bool display_on = false;
    bool button_on = false;
    bool worker_started = false;

    if (s_lock == NULL) {
        SemaphoreHandle_t lock = xSemaphoreCreateMutex();
        if (lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_lock = lock;
    }

    if (!lc_lock_take()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != BOARD_IO_LC_UNINITIALIZED) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = BOARD_IO_LC_INITIALIZING;
    xSemaphoreGive(s_lock);

    rc = build_and_validate_pin_map(&map);
    if (rc != ESP_OK) {
        goto fail;
    }

#if LED_CAP
    rc = board_led_init(CONFIG_BOARD_IO_LED_GPIO,
#if CONFIG_BOARD_IO_LED_ACTIVE_LOW
                        true
#else
                        false
#endif
    );
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Status LED init failed: %s", esp_err_to_name(rc));
        goto fail;
    }
    led_on = true;
    ESP_LOGI(TAG, "Status LED enabled gpio=%d active_low=%d",
             CONFIG_BOARD_IO_LED_GPIO,
#if CONFIG_BOARD_IO_LED_ACTIVE_LOW
             1
#else
             0
#endif
             );
#endif

#if defined(CONFIG_BOARD_IO_DISPLAY_ENABLE)
    rc = board_display_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(rc));
        goto fail;
    }
    display_on = true;
    ESP_LOGI(TAG, "Display enabled backend=none");
#endif

    s_stopped = xSemaphoreCreateBinary();
    if (s_stopped == NULL) {
        rc = ESP_ERR_NO_MEM;
        goto fail;
    }

    BaseType_t created = xTaskCreate(
        worker_loop,
        "board_io",
        CONFIG_BOARD_IO_TASK_STACK_SIZE,
        NULL,
        CONFIG_BOARD_IO_TASK_PRIORITY,
        &s_task);
    if (created != pdPASS) {
        s_task = NULL;
        rc = ESP_ERR_NO_MEM;
        goto fail;
    }
    worker_started = true;

#if BUTTON_CAP
    board_button_config_t btn_cfg = {
        .gpio = map.button_gpio,
        .active_low = map.button_active_low,
        .pull = map.button_pull,
        .debounce_ms = map.debounce_ms,
        .restart_ms = map.restart_ms,
        .factory_ms = map.factory_reset_ms,
    };
    rc = board_button_init(&btn_cfg, s_task);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Button init failed: %s", esp_err_to_name(rc));
        goto fail;
    }
    button_on = true;
#endif

    if (!lc_lock_take()) {
        rc = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    s_state = BOARD_IO_LC_RUNNING;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Worker started");
    return ESP_OK;

fail:
    if (worker_started) {
        notify_worker(BOARD_NOTIFY_STOP);
        if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(STOP_HANDSHAKE_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Worker did not stop during failed init; resources kept");
            return rc;
        }
    }
    if (button_on || (BUTTON_CAP && s_task != NULL)) {
        board_button_deinit();
    }
    if (display_on) {
        board_display_deinit();
    }
    if (led_on) {
        board_led_deinit();
    }
    if (s_stopped != NULL) {
        vSemaphoreDelete(s_stopped);
        s_stopped = NULL;
    }
    s_task = NULL;
    if (lc_lock_take()) {
        s_state = BOARD_IO_LC_UNINITIALIZED;
        xSemaphoreGive(s_lock);
    }
    return rc;
}

esp_err_t board_io_deinit(void)
{
    if (s_lock == NULL) {
        return ESP_OK;
    }

    TaskHandle_t current = xTaskGetCurrentTaskHandle();

    if (!lc_lock_take()) {
        return ESP_ERR_INVALID_STATE;
    }
    board_io_lc_t state = s_state;
    TaskHandle_t task = s_task;
    xSemaphoreGive(s_lock);

    if (state == BOARD_IO_LC_UNINITIALIZED) {
        return ESP_OK;
    }
    if (state == BOARD_IO_LC_INITIALIZING || state == BOARD_IO_LC_STOPPING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (task != NULL && current == task) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lc_lock_take()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_state = BOARD_IO_LC_STOPPING;
    xSemaphoreGive(s_lock);

    board_button_deinit();

    notify_worker(BOARD_NOTIFY_STOP);

    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(STOP_HANDSHAKE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Worker did not stop within %u ms; resources kept",
                 (unsigned)STOP_HANDSHAKE_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    board_display_deinit();
    board_led_deinit();

    vSemaphoreDelete(s_stopped);
    s_stopped = NULL;
    s_task = NULL;

    if (!lc_lock_take()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_handler = NULL;
    s_handler_ctx = NULL;
    s_desired_status = BOARD_STATUS_BOOTING;
    s_state = BOARD_IO_LC_UNINITIALIZED;
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

bool board_io_is_initialized(void)
{
    if (!lc_lock_take()) {
        return false;
    }
    bool running = (s_state == BOARD_IO_LC_RUNNING);
    xSemaphoreGive(s_lock);
    return running;
}

esp_err_t board_io_register_event_handler(
    board_io_event_handler_t handler,
    void *context
)
{
    if (!lc_lock_take()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != BOARD_IO_LC_RUNNING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (handler == NULL) {
        s_handler = NULL;
        s_handler_ctx = NULL;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_handler != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_handler = handler;
    s_handler_ctx = context;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t board_io_set_status(board_status_t status)
{
    if (status >= BOARD_STATUS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lc_lock_take()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != BOARD_IO_LC_RUNNING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_desired_status = status;
    xSemaphoreGive(s_lock);
    notify_worker(BOARD_NOTIFY_STATUS_CHANGED);
    return ESP_OK;
}

esp_err_t board_io_signal(board_signal_t signal)
{
    uint32_t bits;
    switch (signal) {
    case BOARD_SIGNAL_ACTIVITY:
        bits = BOARD_NOTIFY_ACTIVITY;
        break;
    case BOARD_SIGNAL_IDENTIFY:
        bits = BOARD_NOTIFY_IDENTIFY;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (!lc_lock_take()) {
        return ESP_ERR_INVALID_STATE;
    }
    bool running = (s_state == BOARD_IO_LC_RUNNING);
    xSemaphoreGive(s_lock);
    if (!running) {
        return ESP_ERR_INVALID_STATE;
    }
    notify_worker(bits);
    return ESP_OK;
}

esp_err_t board_io_display_update(const board_display_frame_t *frame)
{
    if (!board_display_capability_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!lc_lock_take()) {
        return ESP_ERR_INVALID_STATE;
    }
    bool running = (s_state == BOARD_IO_LC_RUNNING);
    xSemaphoreGive(s_lock);
    if (!running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = board_display_update(frame);
    if (rc == ESP_OK) {
        notify_worker(BOARD_NOTIFY_DISPLAY);
    }
    return rc;
}

esp_err_t board_io_display_set_enabled(bool enabled)
{
    if (!board_display_capability_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!lc_lock_take()) {
        return ESP_ERR_INVALID_STATE;
    }
    bool running = (s_state == BOARD_IO_LC_RUNNING);
    xSemaphoreGive(s_lock);
    if (!running) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t rc = board_display_set_runtime_enabled(enabled);
    if (rc == ESP_OK) {
        notify_worker(BOARD_NOTIFY_DISPLAY);
    }
    return rc;
}
