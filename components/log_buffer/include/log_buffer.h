#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#define LOG_BUFFER_CAPACITY     64
#define LOG_ENTRY_MAX_LEN       192

typedef struct {
    char text[LOG_ENTRY_MAX_LEN];
    long timestamp_ms;
} log_entry_t;

void log_buffer_init(void);
void log_buffer_push(const char *text);
int log_buffer_get_all(log_entry_t *out_entries);
int log_buffer_get_recent(log_entry_t *out_entries, int max_entries);

#endif // LOG_BUFFER_H
