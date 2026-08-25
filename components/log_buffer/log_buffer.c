#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log_buffer.h"

static log_entry_t s_buffer[LOG_BUFFER_CAPACITY];
static int s_head = 0;
static int s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;
static vprintf_like_t s_original_vprintf = &vprintf;
static bool s_log_hook_installed = false;

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
        size_t stored_length = strnlen(line, sizeof(line));
        while (stored_length > 0 &&
               (line[stored_length - 1] == '\n' || line[stored_length - 1] == '\r')) {
            line[--stored_length] = '\0';
        }
        if (stored_length > 0) log_buffer_push(line);
    }
    return result;
}

void log_buffer_init(void)
{
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    memset(s_buffer, 0, sizeof(s_buffer));
    s_head = 0;
    s_count = 0;
    xSemaphoreGive(s_mutex);

    if (!s_log_hook_installed) {
        s_original_vprintf = esp_log_set_vprintf(log_buffer_vprintf);
        s_log_hook_installed = true;
    }
}

void log_buffer_push(const char *text)
{
    if (text == NULL || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;

    strncpy(s_buffer[s_head].text, text, LOG_ENTRY_MAX_LEN - 1);
    s_buffer[s_head].text[LOG_ENTRY_MAX_LEN - 1] = '\0';
    s_buffer[s_head].timestamp_ms = (long)(esp_timer_get_time() / 1000);

    s_head = (s_head + 1) % LOG_BUFFER_CAPACITY;
    if (s_count < LOG_BUFFER_CAPACITY) s_count++;
    xSemaphoreGive(s_mutex);
}

int log_buffer_get_all(log_entry_t *out_entries)
{
    if (out_entries == NULL || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return -1;
    int start = (s_count < LOG_BUFFER_CAPACITY) ? 0 : s_head;
    for (int i = 0; i < s_count; i++) {
        int idx = (start + i) % LOG_BUFFER_CAPACITY;
        out_entries[i] = s_buffer[idx];
    }
    int count = s_count;
    xSemaphoreGive(s_mutex);
    return count;
}

int log_buffer_get_recent(log_entry_t *out_entries, int max_entries)
{
    if (out_entries == NULL || max_entries <= 0 || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return -1;

    int count = s_count < max_entries ? s_count : max_entries;
    int oldest = (s_count < LOG_BUFFER_CAPACITY) ? 0 : s_head;
    int skip = s_count - count;
    for (int i = 0; i < count; i++) {
        int idx = (oldest + skip + i) % LOG_BUFFER_CAPACITY;
        out_entries[i] = s_buffer[idx];
    }
    xSemaphoreGive(s_mutex);
    return count;
}
