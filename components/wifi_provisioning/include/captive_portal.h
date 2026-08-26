#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include <stddef.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "http://" + max IPv4 text (15) + "/" + NUL = 24, rounded up. */
#define CAPTIVE_PORTAL_URI_MAX_LEN 32

/*
 * Formats "http://<ap-ip>/" into uri. Pure helper; unit-testable without
 * Wi-Fi or a netif.
 */
esp_err_t captive_portal_format_uri(const esp_ip4_addr_t *ap_ip, char *uri,
                                    size_t len);

/*
 * URI stored by the last successful format inside the component (static
 * storage: ESP-IDF keeps the pointer passed via DHCP option 114 for the
 * whole DHCPS lifetime). NULL before the first successful format.
 */
const char *captive_portal_uri(void);

/*
 * Advertises the portal URI via DHCP option 114 on the SoftAP netif:
 * stop DHCPS -> set option -> start DHCPS. Call only while no station holds
 * a lease (boot-time provisioning entry satisfies this); restarting DHCPS
 * later would drop client leases. Failure is non-fatal by policy: callers
 * warn and keep the DNS/HTTP funnel.
 */
esp_err_t captive_portal_configure_dhcp(esp_netif_t *ap_netif,
                                        const esp_ip4_addr_t *ap_ip);

#ifdef __cplusplus
}
#endif

#endif // CAPTIVE_PORTAL_H
