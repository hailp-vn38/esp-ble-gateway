#ifndef DNS_HIJACK_H
#define DNS_HIJACK_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dns_hijack_start(const esp_ip4_addr_t *redirect_ip);
esp_err_t dns_hijack_stop(void);
bool dns_hijack_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // DNS_HIJACK_H
