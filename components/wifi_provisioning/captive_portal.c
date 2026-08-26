#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "captive_portal.h"

static const char *TAG = "captive_portal";

/* Component-lifetime storage: option 114 keeps the caller's pointer. */
static char s_portal_uri[CAPTIVE_PORTAL_URI_MAX_LEN];

esp_err_t captive_portal_format_uri(const esp_ip4_addr_t *ap_ip, char *uri,
                                    size_t len)
{
    if (ap_ip == NULL || uri == NULL || ap_ip->addr == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(uri, len, "http://" IPSTR "/", IP2STR(ap_ip));
    if (written <= 0 || (size_t)written >= len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

const char *captive_portal_uri(void)
{
    return s_portal_uri[0] != '\0' ? s_portal_uri : NULL;
}

esp_err_t captive_portal_configure_dhcp(esp_netif_t *ap_netif,
                                        const esp_ip4_addr_t *ap_ip)
{
    esp_err_t err = captive_portal_format_uri(ap_ip, s_portal_uri,
                                              sizeof(s_portal_uri));
    if (err != ESP_OK) return err;

#if CONFIG_WIFI_PROV_CAPTIVE_DHCP_OPTION_114
    esp_err_t stop_err = esp_netif_dhcps_stop(ap_netif);
    if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        /* Unknown server state: attempt to restore leases before failing. */
        esp_netif_dhcps_start(ap_netif);
        ESP_LOGE(TAG, "Could not stop DHCPS: %s", esp_err_to_name(stop_err));
        return stop_err;
    }

    esp_err_t option_err = esp_netif_dhcps_option(
        ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
        s_portal_uri, strlen(s_portal_uri));

    esp_err_t start_err = esp_netif_dhcps_start(ap_netif);

    if (option_err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP option 114 unavailable (%s); DNS-only portal",
                 esp_err_to_name(option_err));
        return option_err;
    }
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "Could not restart DHCPS: %s",
                 esp_err_to_name(start_err));
        return start_err;
    }
    ESP_LOGI(TAG, "DHCP option 114 advertised: %s", s_portal_uri);
#else
    /* Feature off: keep the URI for diagnostics only. */
#endif
    return ESP_OK;
}
