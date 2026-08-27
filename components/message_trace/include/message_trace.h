#ifndef MESSAGE_TRACE_H
#define MESSAGE_TRACE_H

#include <stddef.h>
#include <stdint.h>

#include "cbor_codec.h"

/* Thread-safe frame ID allocation. Returns nonzero boot-local ID. */
uint32_t message_trace_next_frame_id(void);

/* TX trace points — called from BLE central send path. */
void message_trace_tx_decoded(uint32_t frame_id,
                              const char *device_id,
                              const gw_message_t *message,
                              size_t encoded_len);

void message_trace_tx_raw(uint32_t frame_id,
                          const uint8_t *data,
                          size_t len);

void message_trace_tx_result(uint32_t frame_id, int result);

/* RX trace points — called from BLE notify worker (NOT from host callback). */
void message_trace_rx_raw(uint32_t frame_id,
                          const char *device_id,
                          const uint8_t *data,
                          size_t len);

void message_trace_rx_decoded(uint32_t frame_id,
                              const char *device_id,
                              const gw_message_t *message);

void message_trace_rx_decode_error(uint32_t frame_id,
                                   const char *device_id,
                                   int decode_result);

#endif // MESSAGE_TRACE_H
