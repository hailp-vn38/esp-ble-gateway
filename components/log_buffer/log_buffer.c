#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <stdatomic.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "log_buffer.h"

#define LOG_BUFFER_READ_LOCK_TIMEOUT_MS 20U

static log_entry_t s_buffer[LOG_BUFFER_CAPACITY];
static size_t s_head;
static size_t s_count;
static SemaphoreHandle_t s_mutex;

static vprintf_like_t s_original_vprintf = &vprintf;
static bool s_hook_installed;
static bool s_initialized;

static atomic_uint_fast32_t s_dropped_count;

static int log_buffer_vprintf(const char *format, va_list args)
{
    va_list serial_args;
    va_copy(serial_args, args);
    int result = s_original_vprintf(format, serial_args);
    va_end(serial_args);

    char line[LOG_ENTRY_MAX_LEN];
    va_list buffer_args;
    va_copy(buffer_args, args);
    int length = vsnprintf(line, sizeof(line), format, buffer_args);
    va_end(buffer_args);

    if (length > 0) {
        (void)log_buffer_push(line);
    }
    return result;
}

esp_err_t log_buffer_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_mutex = mutex;
    s_head = 0;
    s_count = 0;
    atomic_store_explicit(&s_dropped_count, 0U, memory_order_relaxed);

    if (!s_hook_installed) {
        s_original_vprintf = esp_log_set_vprintf(log_buffer_vprintf);
        s_hook_installed = true;
    }

    s_initialized = true;
    return ESP_OK;
}

void log_buffer_clear(void)
{
    if (!s_initialized || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(LOG_BUFFER_READ_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }

    s_head = 0;
    s_count = 0;
    xSemaphoreGive(s_mutex);
}

bool log_buffer_push(const char *text)
{
    if (text == NULL || !s_initialized || s_mutex == NULL) {
        return false;
    }

    size_t length = strnlen(text, LOG_ENTRY_MAX_LEN - 1U);
    while (length > 0U &&
           (text[length - 1U] == '\n' || text[length - 1U] == '\r')) {
        length--;
    }
    if (length == 0U) {
        return false;
    }

    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        atomic_fetch_add_explicit(&s_dropped_count, 1U, memory_order_relaxed);
        return false;
    }

    memcpy(s_buffer[s_head].text, text, length);
    s_buffer[s_head].text[length] = '\0';
    s_buffer[s_head].uptime_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    s_head = (s_head + 1U) % LOG_BUFFER_CAPACITY;
    if (s_count < LOG_BUFFER_CAPACITY) {
        s_count++;
    }

    xSemaphoreGive(s_mutex);
    return true;
}

int log_buffer_get_recent(log_entry_t *out_entries, size_t capacity)
{
    if (out_entries == NULL || capacity == 0U || !s_initialized ||
        s_mutex == NULL ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(LOG_BUFFER_READ_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return -1;
    }

    size_t count = (s_count < capacity) ? s_count : capacity;
    size_t oldest = (s_count < LOG_BUFFER_CAPACITY) ? 0U : s_head;
    size_t skip = s_count - count;
    for (size_t i = 0; i < count; i++) {
        out_entries[i] = s_buffer[(oldest + skip + i) % LOG_BUFFER_CAPACITY];
    }

    xSemaphoreGive(s_mutex);
    return (int)count;
}

uint32_t log_buffer_get_dropped_count(void)
{
    return (uint32_t)atomic_load_explicit(&s_dropped_count, memory_order_relaxed);
}

#ifdef CONFIG_LOG_BUFFER_TEST_HELPERS

SemaphoreHandle_t log_buffer_test_get_ring_mutex(void)
{
    return s_mutex;
}

void log_buffer_test_reset_metrics(void)
{
    atomic_store_explicit(&s_dropped_count, 0U, memory_order_relaxed);
}

#endif // CONFIG_LOG_BUFFER_TEST_HELPERS
