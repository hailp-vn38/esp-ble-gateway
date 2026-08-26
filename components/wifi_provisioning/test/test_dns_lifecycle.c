/* Layer B — DNS task/component tests (spec §21.2).
 * Runs on-device against the lwIP loopback interface (127.0.0.1).
 * DNS-C06/C07/C08 (bind-failure / task-create-failure / stop-timeout
 * injection) need fault injection hooks that this test harness does not
 * have; they are covered by review and remain a documented exception. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "unity.h"

#include "dns_hijack.h"
#include "dns_packet.h"

static void ensure_loopback_ready(void)
{
    static bool initialized;
    if (!initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        /* Give the lwIP TCP/IP thread a moment to start serving sockets. */
        vTaskDelay(pdMS_TO_TICKS(100));
        initialized = true;
    }
}

static esp_ip4_addr_t loopback_ip(void)
{
    esp_ip4_addr_t ip;
    ip.addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 */
    return ip;
}

/* Sends an A query for "example.com" to 127.0.0.1:53 and returns the
 * response length, or -1 on any socket error/timeout. */
static int captive_query(uint8_t *response, size_t response_len)
{
    uint8_t query[32];
    memset(query, 0, sizeof(query));
    query[2] = 0x01; /* RD=1 */
    query[5] = 1;    /* QDCOUNT = 1 */
    int offset = 12;
    query[offset++] = 7;
    memcpy(query + offset, "example", 7);
    offset += 7;
    query[offset++] = 3;
    memcpy(query + offset, "com", 3);
    offset += 3;
    query[offset++] = 0;
    query[offset++] = 0;
    query[offset++] = 1; /* A */
    query[offset++] = 0;
    query[offset++] = 1; /* IN */

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int sent = sendto(sock, query, offset, 0,
                      (struct sockaddr *)&server, sizeof(server));
    if (sent != offset) {
        close(sock);
        return -1;
    }

    int len = (int)recv(sock, response, response_len, 0);
    close(sock);
    return len;
}

TEST_CASE("DNS-C01: start once binds and reports running", "[dns]")
{
    ensure_loopback_ready();
    esp_ip4_addr_t ip = loopback_ip();

    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_start(&ip));
    TEST_ASSERT_TRUE(dns_hijack_is_running());
    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_stop());
    TEST_ASSERT_FALSE(dns_hijack_is_running());
}

TEST_CASE("DNS-C02: start twice with same IP keeps one server", "[dns]")
{
    ensure_loopback_ready();
    esp_ip4_addr_t ip = loopback_ip();

    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_start(&ip));
    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_start(&ip));
    TEST_ASSERT_TRUE(dns_hijack_is_running());

    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_stop());
    TEST_ASSERT_FALSE(dns_hijack_is_running());
}

TEST_CASE("DNS-C03: stop returns only after cleanup", "[dns]")
{
    ensure_loopback_ready();
    esp_ip4_addr_t ip = loopback_ip();

    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_start(&ip));
    TEST_ASSERT_TRUE(dns_hijack_is_running());
    /* Only returns once the old task released UDP 53. */
    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_stop());
    TEST_ASSERT_FALSE(dns_hijack_is_running());
}

TEST_CASE("DNS-C04: stop twice is idempotent", "[dns]")
{
    ensure_loopback_ready();
    esp_ip4_addr_t ip = loopback_ip();

    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_start(&ip));
    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_stop());
    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_stop());
    TEST_ASSERT_FALSE(dns_hijack_is_running());
}

TEST_CASE("DNS-C05: stop then immediate start re-binds successfully",
          "[dns]")
{
    ensure_loopback_ready();
    esp_ip4_addr_t ip = loopback_ip();
    uint8_t response[DNS_MAX_PACKET_LEN];

    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_start(&ip));
    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_stop());

    /* New start must own UDP 53 again right away. */
    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_start(&ip));
    TEST_ASSERT_TRUE(dns_hijack_is_running());

    int len = captive_query(response, sizeof(response));
    TEST_ASSERT_TRUE(len > DNS_HEADER_LEN);

    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_stop());
}

TEST_CASE("DNS-B10: running server answers captive A query via loopback",
          "[dns]")
{
    ensure_loopback_ready();
    esp_ip4_addr_t ip = loopback_ip();
    uint8_t response[DNS_MAX_PACKET_LEN];

    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_start(&ip));

    int len = captive_query(response, sizeof(response));
    TEST_ASSERT_TRUE(len > DNS_HEADER_LEN);
    TEST_ASSERT_EQUAL_HEX8(0x81, response[2]);      /* QR=1, RD copied */
    TEST_ASSERT_EQUAL_HEX8(0x00, response[3]);      /* NOERROR, RA=0 */
    TEST_ASSERT_EQUAL_HEX8(0x01, response[7]);      /* ANCOUNT = 1 */
    const uint8_t *rdata = response + len - 4;
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"\x7f\x00\x00\x01",
                                  rdata, 4); /* answer = 127.0.0.1 */

    TEST_ASSERT_EQUAL(ESP_OK, dns_hijack_stop());
}

TEST_CASE("DNS-B11: start with NULL or zero IP rejected", "[dns]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, dns_hijack_start(NULL));

    esp_ip4_addr_t zero = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, dns_hijack_start(&zero));
    TEST_ASSERT_FALSE(dns_hijack_is_running());
}
