#include "board_button.h"

#include "esp_log.h"

#include "driver/gpio.h"

#include "board_button_fsm.h"
#include "board_led.h"

static const char *TAG = "board_io";

static bool s_enabled;
static board_button_config_t s_cfg;
static TaskHandle_t s_worker;

static board_button_fsm_t s_fsm;
static volatile bool s_edge_pending;
static bool s_debounce_pending;
static uint64_t s_debounce_deadline;
static uint64_t s_hold_restart_dl;
static uint64_t s_hold_factory_dl;

static void apply_fsm_result(const board_button_fsm_result_t *r, uint64_t now_ms)
{
    board_led_set_armed(r->overlay, now_ms);
}

static void button_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    TaskHandle_t task = s_worker;
    if (task != NULL) {
        xTaskNotifyFromISR(task, BOARD_NOTIFY_BUTTON_EDGE, eSetBits, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

int board_button_gpio_reader(int gpio)
{
    return gpio_get_level((gpio_num_t)gpio);
}

esp_err_t board_button_init(const board_button_config_t *cfg, TaskHandle_t worker_task)
{
    if (cfg == NULL || cfg->gpio < 0 || worker_task == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_int_type_t intr = GPIO_INTR_ANYEDGE;
    gpio_pull_mode_t pull = GPIO_FLOATING;
    if (cfg->pull == BOARD_PIN_PULL_UP) {
        pull = GPIO_PULLUP_ONLY;
    } else if (cfg->pull == BOARD_PIN_PULL_DOWN) {
        pull = GPIO_PULLDOWN_ONLY;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << cfg->gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (pull == GPIO_PULLUP_ONLY) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (pull == GPIO_PULLDOWN_ONLY) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = intr,
    };
    esp_err_t rc = gpio_config(&io);
    if (rc != ESP_OK) {
        return rc;
    }

    rc = gpio_install_isr_service(0);
    if (rc == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "GPIO ISR service already installed; reusing");
    } else if (rc != ESP_OK) {
        return rc;
    }

    rc = gpio_isr_handler_add((gpio_num_t)cfg->gpio, button_isr, NULL);
    if (rc != ESP_OK) {
        return rc;
    }

    s_enabled = true;
    s_cfg = *cfg;
    s_worker = worker_task;
    s_edge_pending = false;
    s_debounce_pending = false;
    s_debounce_deadline = 0;
    s_hold_restart_dl = UINT64_MAX;
    s_hold_factory_dl = UINT64_MAX;
    board_button_fsm_init(&s_fsm);

    ESP_LOGI(TAG, "Button enabled gpio=%d active_low=%d", cfg->gpio, cfg->active_low);
    return ESP_OK;
}

void board_button_deinit(void)
{
    if (!s_enabled) {
        return;
    }
    gpio_intr_disable((gpio_num_t)s_cfg.gpio);
    gpio_isr_handler_remove((gpio_num_t)s_cfg.gpio);
    s_enabled = false;
    s_worker = NULL;
    s_edge_pending = false;
    s_debounce_pending = false;
}

bool board_button_is_enabled(void)
{
    return s_enabled;
}

void board_button_on_edge_notify(void)
{
    s_edge_pending = true;
}

size_t board_button_process(
    uint64_t now_ms,
    board_button_level_reader_t reader,
    board_io_event_t *out_events,
    size_t max_events
)
{
    if (!s_enabled) {
        return 0;
    }

    size_t emitted = 0;

    if (s_edge_pending) {
        s_edge_pending = false;
        s_debounce_pending = true;
        s_debounce_deadline = board_deadline_add_ms(now_ms, s_cfg.debounce_ms);
    }

    if (s_debounce_pending && now_ms >= s_debounce_deadline) {
        s_debounce_pending = false;
        int level = reader(s_cfg.gpio);
        bool pressed = s_cfg.active_low ? (level == 0) : (level != 0);
        if (pressed != s_fsm.pressed) {
            bool was_pressed = s_fsm.pressed;
            board_button_fsm_result_t r =
                board_button_fsm_feed(&s_fsm, pressed, now_ms,
                                      s_cfg.restart_ms, s_cfg.factory_ms);
            apply_fsm_result(&r, now_ms);
            if (!was_pressed) {
                s_hold_restart_dl = board_deadline_add_ms(now_ms, s_cfg.restart_ms);
                s_hold_factory_dl = board_deadline_add_ms(now_ms, s_cfg.factory_ms);
            } else {
                s_hold_restart_dl = UINT64_MAX;
                s_hold_factory_dl = UINT64_MAX;
                if (r.has_event && emitted < max_events) {
                    out_events[emitted++] = r.event;
                }
            }
        }
    }

    if (s_fsm.pressed) {
        bool need_feed = false;
        if (now_ms >= s_hold_factory_dl &&
            s_fsm.state != BOARD_BUTTON_FSM_FACTORY_ARMED) {
            need_feed = true;
        } else if (now_ms >= s_hold_restart_dl &&
                   s_fsm.state == BOARD_BUTTON_FSM_PRESSED) {
            need_feed = true;
        }
        if (need_feed) {
            board_button_fsm_result_t r =
                board_button_fsm_feed(&s_fsm, true, now_ms,
                                      s_cfg.restart_ms, s_cfg.factory_ms);
            apply_fsm_result(&r, now_ms);
            if (s_fsm.state == BOARD_BUTTON_FSM_RESTART_ARMED) {
                s_hold_restart_dl = UINT64_MAX;
            } else if (s_fsm.state == BOARD_BUTTON_FSM_FACTORY_ARMED) {
                s_hold_restart_dl = UINT64_MAX;
                s_hold_factory_dl = UINT64_MAX;
            }
        }
    }

    return emitted;
}

bool board_button_next_deadline(uint64_t now_ms, uint64_t *deadline_ms)
{
    if (!s_enabled) {
        return false;
    }

    uint64_t best = UINT64_MAX;

    if (s_edge_pending && !s_debounce_pending) {
        best = now_ms + s_cfg.debounce_ms;
    } else if (s_debounce_pending) {
        best = s_debounce_deadline;
    }

    if (s_fsm.pressed) {
        if (s_hold_restart_dl < best) {
            best = s_hold_restart_dl;
        }
        if (s_hold_factory_dl < best) {
            best = s_hold_factory_dl;
        }
    }

    if (best != UINT64_MAX) {
        *deadline_ms = best;
        return true;
    }
    return false;
}
