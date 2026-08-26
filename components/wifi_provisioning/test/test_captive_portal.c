/* Captive portal URI builder + DHCP option 114 arg validation (spec v3 §16.3).
 * Pure checks only; real DHCPS packet verification is a device-level test. */

#include <string.h>

#include "unity.h"

#include "captive_portal.h"

static void make_ip(esp_ip4_addr_t *ip, uint8_t a, uint8_t b, uint8_t c,
                    uint8_t d)
{
    const uint8_t bytes[4] = {a, b, c, d};
    memcpy(&ip->addr, bytes, sizeof(ip->addr));
}

TEST_CASE("portal uri formats typical softap ip", "[wifi_provisioning]")
{
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);
    char uri[CAPTIVE_PORTAL_URI_MAX_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, captive_portal_format_uri(&ip, uri, sizeof(uri)));
    TEST_ASSERT_EQUAL_STRING("http://192.168.4.1/", uri);
}

TEST_CASE("portal uri formats max-length ipv4", "[wifi_provisioning]")
{
    esp_ip4_addr_t ip;
    make_ip(&ip, 255, 255, 255, 255);
    char uri[CAPTIVE_PORTAL_URI_MAX_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, captive_portal_format_uri(&ip, uri, sizeof(uri)));
    TEST_ASSERT_EQUAL_STRING("http://255.255.255.255/", uri);
}

TEST_CASE("portal uri rejects undersized buffer", "[wifi_provisioning]")
{
    esp_ip4_addr_t ip;
    make_ip(&ip, 10, 0, 0, 2);
    char tiny[8];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      captive_portal_format_uri(&ip, tiny, sizeof(tiny)));
}

TEST_CASE("portal uri rejects invalid input", "[wifi_provisioning]")
{
    char uri[CAPTIVE_PORTAL_URI_MAX_LEN];
    esp_ip4_addr_t zero = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      captive_portal_format_uri(NULL, uri, sizeof(uri)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      captive_portal_format_uri(&zero, uri, sizeof(uri)));
}

#if CONFIG_WIFI_PROV_CAPTIVE_DHCP_OPTION_114
TEST_CASE("dhcp configure rejects null netif", "[wifi_provisioning]")
{
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      captive_portal_configure_dhcp(NULL, &ip));
}
#endif
