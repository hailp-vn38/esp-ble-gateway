#ifndef GATEWAY_EVENTS_H
#define GATEWAY_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#include "cbor_codec.h"
#include "esp_err.h"

#define GATEWAY_EVENT_MAX_LISTENERS 4

/* ── Event types ──────────────────────────────────────────────────── */

typedef enum {
    GW_EVENT_DEVICE_CHANGED = 0,
    GW_EVENT_DEVICE_CONNECTION,
    GW_EVENT_DEVICE_SCHEMA,
    GW_EVENT_FEATURE_STATE,
    GW_EVENT_RESYNC_REQUIRED,
} gateway_event_type_t;

typedef enum {
    GW_EVENT_VALUE_NONE = 0,
    GW_EVENT_VALUE_BOOL,
    GW_EVENT_VALUE_INT,
} gateway_event_value_kind_t;

/* ── Event struct (fixed-size, no heap allocation) ────────────────── */

typedef struct {
    uint32_t seq;
    gateway_event_type_t type;

    char device_id[GW_MSG_DEVICE_ID_LEN];
    char feature_id[GW_FEATURE_ID_LEN];

    uint8_t property_id;
    gateway_event_value_kind_t value_kind;

    bool bool_value;
    int32_t int_value;

    uint32_t schema_revision;
    int64_t updated_at_ms;
} gateway_event_t;

/* ── Listener callback ────────────────────────────────────────────── */

typedef void (*gateway_event_listener_t)(const gateway_event_t *event,
                                         void *context);

/* ── Public API ───────────────────────────────────────────────────── */

/**
 * Initialize the event bus. Must be called once before any producer.
 */
esp_err_t gateway_events_init(void);

/**
 * Register a listener. Returns ESP_ERR_NO_MEM if full.
 */
esp_err_t gateway_events_register(gateway_event_listener_t listener,
                                  void *context);

/**
 * Publish an event. Assigns monotonic seq and copies event values
 * before calling listeners. Never malloc, never cJSON, never BLE call.
 */
void gateway_events_publish(gateway_event_t *event);

/**
 * Get the current global sequence number.
 */
uint32_t gateway_events_current_seq(void);

/**
 * Reset all state (test only).
 */
void gateway_events_reset_for_test(void);

#endif /* GATEWAY_EVENTS_H */
