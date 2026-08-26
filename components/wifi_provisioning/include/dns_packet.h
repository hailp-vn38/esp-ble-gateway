#ifndef DNS_PACKET_H
#define DNS_PACKET_H

#include <stdint.h>

#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DNS_MAX_PACKET_LEN 512
#define DNS_HEADER_LEN 12
#define DNS_ANSWER_LEN 16
#define DNS_ANSWER_TTL_S 30

/*
 * Parses the query header/question and writes a captive response in-place.
 * Returns response length, or -1 when the packet must be dropped
 * (malformed or a non-query packet).
 */
int dns_build_response(uint8_t *packet, int query_len,
                       const esp_ip4_addr_t *redirect_ip);

#ifdef __cplusplus
}
#endif

#endif // DNS_PACKET_H
