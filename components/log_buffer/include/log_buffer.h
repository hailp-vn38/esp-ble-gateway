#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_BUFFER_CAPACITY 64U
#define LOG_ENTRY_MAX_LEN   192U

typedef struct {
    char text[LOG_ENTRY_MAX_LEN];
    uint64_t uptime_ms;
} log_entry_t;

/**
 * Initialize the RAM log buffer and install the ESP-IDF vprintf hook.
 *
 * Idempotent: a second call returns ESP_OK without clearing entries or
 * installing another hook. The previous vprintf handler is captured at
 * install time and forwarded to on every log line.
 *
 * The hook lives until firmware reset; there is no production deinit.
 *
 * Returns ESP_OK, or ESP_ERR_NO_MEM if the ring mutex cannot be created.
 */
esp_err_t log_buffer_init(void);

/**
 * Clear stored entries while keeping the component initialized and keeping
 * the ESP-IDF log hook installed. No-op if not initialized.
 *
 * Does not reset the dropped-entry counter.
 */
void log_buffer_clear(void);

/**
 * Push one already-formatted text entry into the RAM ring.
 *
 * Trailing '\n'/'\r' are trimmed; entries that become empty after trimming
 * are rejected. The capture operation never waits for the ring mutex: if
 * the mutex is contended the entry is dropped and the dropped counter is
 * incremented.
 *
 * Returns true only if the entry is committed to the ring.
 */
bool log_buffer_push(const char *text);

/**
 * Copy up to `capacity` most recent entries into `out_entries`.
 *
 * Ordering: oldest selected entry -> newest selected entry.
 *
 * Note: logging from ISR context is unsupported (FreeRTOS mutexes cannot
 * be taken from ISRs); ESP_LOGx must not be called from ISRs anyway.
 *
 * Returns:
 *   >= 0 : number of copied entries
 *   -1   : invalid argument (NULL output / zero capacity), component not
 *          initialized, or read lock timeout
 */
int log_buffer_get_recent(log_entry_t *out_entries, size_t capacity);

/**
 * Number of valid entries dropped due to writer contention.
 */
uint32_t log_buffer_get_dropped_count(void);

#ifdef CONFIG_LOG_BUFFER_TEST_HELPERS
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * Test-only: expose the ring mutex so tests can force deterministic writer
 * contention. Not part of the production API.
 */
SemaphoreHandle_t log_buffer_test_get_ring_mutex(void);

/**
 * Test-only: reset the dropped counter to zero. Not part of the production
 * API.
 */
void log_buffer_test_reset_metrics(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // LOG_BUFFER_H
