#include <string.h>
#include "esp_timer.h"
#include "log_buffer.h"

static log_entry_t s_buffer[LOG_BUFFER_CAPACITY];
static int s_head = 0;
static int s_count = 0;

void log_buffer_init(void)
{
    memset(s_buffer, 0, sizeof(s_buffer));
    s_head = 0;
    s_count = 0;
}

void log_buffer_push(const char *text)
{
    if (text == NULL) return;

    strncpy(s_buffer[s_head].text, text, LOG_ENTRY_MAX_LEN - 1);
    s_buffer[s_head].text[LOG_ENTRY_MAX_LEN - 1] = '\0';
    s_buffer[s_head].timestamp_ms = (long)(esp_timer_get_time() / 1000);

    s_head = (s_head + 1) % LOG_BUFFER_CAPACITY;
    if (s_count < LOG_BUFFER_CAPACITY) s_count++;
}

int log_buffer_get_all(log_entry_t *out_entries)
{
    int start = (s_count < LOG_BUFFER_CAPACITY) ? 0 : s_head;
    for (int i = 0; i < s_count; i++) {
        int idx = (start + i) % LOG_BUFFER_CAPACITY;
        out_entries[i] = s_buffer[idx];
    }
    return s_count;
}
