/* Layer A — pure DNS parser/response-builder unit tests (spec §21.1).
 * No sockets or Wi-Fi hardware required; exercises dns_build_response()
 * from dns_packet.c only. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "dns_packet.h"

static void make_ip(esp_ip4_addr_t *ip, uint8_t a, uint8_t b, uint8_t c,
                    uint8_t d)
{
    const uint8_t bytes[4] = {a, b, c, d};
    memcpy(&ip->addr, bytes, sizeof(ip->addr));
}

/* Encodes a DNS query: header + QNAME from dotted name + QTYPE + QCLASS. */
static int build_query(uint8_t *pkt, uint16_t flags, int qdcount,
                       const char *dotted_name, uint16_t qtype,
                       uint16_t qclass)
{
    memset(pkt, 0, 12);
    pkt[2] = (uint8_t)(flags >> 8);
    pkt[3] = (uint8_t)(flags & 0xff);
    pkt[4] = (uint8_t)(qdcount >> 8);
    pkt[5] = (uint8_t)(qdcount & 0xff);

    int offset = 12;
    const char *p = dotted_name;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t label_len = dot != NULL ? (size_t)(dot - p) : strlen(p);
        pkt[offset++] = (uint8_t)label_len;
        memcpy(pkt + offset, p, label_len);
        offset += (int)label_len;
        p += label_len;
        if (dot != NULL) p++;
    }
    pkt[offset++] = 0;
    pkt[offset++] = (uint8_t)(qtype >> 8);
    pkt[offset++] = (uint8_t)(qtype & 0xff);
    pkt[offset++] = (uint8_t)(qclass >> 8);
    pkt[offset++] = (uint8_t)(qclass & 0xff);
    return offset;
}

static bool answer_ip_is(const uint8_t *resp, int len, const esp_ip4_addr_t *ip)
{
    if (len < DNS_HEADER_LEN) return false;
    uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
    if (ancount != 1) return false;

    /* Answer rdata is the final 4 bytes of the response. */
    const uint8_t *rdata = resp + len - 4;
    return memcmp(rdata, &ip->addr, sizeof(ip->addr)) == 0;
}

TEST_CASE("DNS-U01: valid A query with QDCOUNT=1 parses", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    int query_len = build_query(pkt, 0x0100, 1, "example.com",
                                1 /* A */, 1 /* IN */);
    TEST_ASSERT_EQUAL(12 + 13 + 4, query_len);

    int len = dns_build_response(pkt, query_len, &ip);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_HEX8(0x81, pkt[2]); /* QR=1, RD copied */
    TEST_ASSERT_EQUAL_HEX8(0x00, pkt[3]); /* NOERROR, RA=0 */
    TEST_ASSERT_TRUE(answer_ip_is(pkt, len, &ip));
}

TEST_CASE("DNS-U02: QDCOUNT=0 answered NOTIMP", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    build_query(pkt, 0x0100, 0, "example.com", 1, 1);
    pkt[5] = 0; /* QDCOUNT = 0 */

    int len = dns_build_response(pkt, 28, &ip);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_HEX8(0x04, pkt[3] & 0x0f); /* NOTIMP */
}

TEST_CASE("DNS-U03: QDCOUNT=2 answered NOTIMP", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    build_query(pkt, 0x0100, 2, "example.com", 1, 1);

    int len = dns_build_response(pkt, 29, &ip);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_HEX8(0x04, pkt[3] & 0x0f); /* NOTIMP */
}

TEST_CASE("DNS-U04: label length beyond packet rejected without OOB", "[dns]")
{
    uint8_t pkt[64];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    build_query(pkt, 0x0100, 1, "example.com", 1, 1);
    pkt[12] = 200; /* claims 200-byte label inside a short packet */

    TEST_ASSERT_EQUAL(-1, dns_build_response(pkt, 29, &ip));
}

TEST_CASE("DNS-U05: compressed QNAME pointer in request rejected", "[dns]")
{
    uint8_t pkt[64];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    build_query(pkt, 0x0100, 1, "example.com", 1, 1);
    pkt[12] |= 0xc0; /* first label byte is a compression pointer */

    TEST_ASSERT_EQUAL(-1, dns_build_response(pkt, 29, &ip));
}

TEST_CASE("DNS-U06: QR=1 packet dropped as non-query", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    build_query(pkt, 0x8100, 1, "example.com", 1, 1); /* QR=1 */

    TEST_ASSERT_EQUAL(-1, dns_build_response(pkt, 29, &ip));
}

TEST_CASE("DNS-U07: non-zero OPCODE answered NOTIMP", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    build_query(pkt, 0x0900, 1, "example.com", 1, 1); /* OPCODE=1 */

    int len = dns_build_response(pkt, 29, &ip);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_HEX8(0x04, pkt[3] & 0x0f); /* NOTIMP */
}

TEST_CASE("DNS-U08: A/IN question gets exactly one A answer", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    int query_len = build_query(pkt, 0x0100, 1, "gateway.local", 1, 1);
    int len = dns_build_response(pkt, query_len, &ip);
    TEST_ASSERT_TRUE(len > query_len);
    TEST_ASSERT_TRUE(answer_ip_is(pkt, len, &ip));

    /* Answer type must be A/IN with TTL 30 and RDLENGTH 4. */
    const uint8_t *answer = pkt + query_len;
    TEST_ASSERT_EQUAL_HEX8(0xc0, answer[0]);
    TEST_ASSERT_EQUAL_HEX8(0x0c, answer[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, answer[2]);
    TEST_ASSERT_EQUAL_HEX8(0x01, answer[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00, answer[4]);
    TEST_ASSERT_EQUAL_HEX8(0x01, answer[5]);
    TEST_ASSERT_EQUAL(30, (answer[8] << 24) | (answer[9] << 16) |
                              (answer[10] << 8) | answer[11]);
    TEST_ASSERT_EQUAL_HEX8(0x00, answer[12]);
    TEST_ASSERT_EQUAL_HEX8(0x04, answer[13]);
}

TEST_CASE("DNS-U09: AAAA/IN gets NOERROR with zero answers", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    int query_len = build_query(pkt, 0x0100, 1, "example.com",
                                28 /* AAAA */, 1);
    int len = dns_build_response(pkt, query_len, &ip);
    TEST_ASSERT_EQUAL(query_len, len); /* question echoed back, no answer */
    TEST_ASSERT_EQUAL_HEX8(0x00, pkt[3]); /* NOERROR, RA=0 */
    TEST_ASSERT_EQUAL_HEX8(0x00, pkt[7]); /* ANCOUNT low byte */
}

TEST_CASE("DNS-U10: other QTYPE/IN gets NOERROR with zero answers", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    int query_len = build_query(pkt, 0x0100, 1, "example.com",
                                16 /* TXT */, 1);
    int len = dns_build_response(pkt, query_len, &ip);
    TEST_ASSERT_EQUAL(query_len, len);
    TEST_ASSERT_EQUAL_HEX8(0x00, pkt[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00, pkt[7]);
}

TEST_CASE("DNS-U11: QCLASS != IN answered NOTIMP", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    int query_len = build_query(pkt, 0x0100, 1, "example.com", 1, 3 /* CH */);
    int len = dns_build_response(pkt, query_len, &ip);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_HEX8(0x04, pkt[3] & 0x0f); /* NOTIMP */
}

TEST_CASE("DNS-U12: local.adguard.org answered NXDOMAIN", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    int query_len = build_query(pkt, 0x0100, 1, "local.adguard.org", 1, 1);
    int len = dns_build_response(pkt, query_len, &ip);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_HEX8(0x03, pkt[3] & 0x0f); /* NXDOMAIN */
    TEST_ASSERT_FALSE(answer_ip_is(pkt, len, &ip));
}

TEST_CASE("DNS-U13: local.adguard.com answered NXDOMAIN", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    int query_len = build_query(pkt, 0x0100, 1, "local.adguard.com", 1, 1);
    int len = dns_build_response(pkt, query_len, &ip);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_HEX8(0x03, pkt[3] & 0x0f); /* NXDOMAIN */
}

TEST_CASE("DNS-U14: response flags QR=1, RA=0, Z=0, RD copied", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    /* RD=1 request. */
    int rd1_len = build_query(pkt, 0x0100, 1, "example.com", 28, 1);
    TEST_ASSERT_TRUE(dns_build_response(pkt, rd1_len, &ip) > 0);
    TEST_ASSERT_EQUAL_HEX8(0x81, pkt[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, pkt[3]);

    /* RD=0 request. */
    int rd0_len = build_query(pkt, 0x0000, 1, "example.com", 28, 1);
    TEST_ASSERT_TRUE(dns_build_response(pkt, rd0_len, &ip) > 0);
    TEST_ASSERT_EQUAL_HEX8(0x80, pkt[2]);
}

TEST_CASE("DNS-U15: answer uses passed redirect IP, not a constant", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t captive;
    esp_ip4_addr_t other;

    make_ip(&captive, 10, 20, 30, 40);
    make_ip(&other, 192, 168, 4, 1);

    int query_len = build_query(pkt, 0x0100, 1, "example.com", 1, 1);
    int len = dns_build_response(pkt, query_len, &captive);
    TEST_ASSERT_TRUE(answer_ip_is(pkt, len, &captive));
    TEST_ASSERT_FALSE(answer_ip_is(pkt, len, &other));
}

TEST_CASE("DNS-B01: A answer that would overflow packet is dropped", "[dns]")
{
    uint8_t pkt[DNS_MAX_PACKET_LEN];
    esp_ip4_addr_t ip;
    make_ip(&ip, 192, 168, 4, 1);

    /* Pad the QNAME so question end leaves < 16 bytes for the answer. */
    int offset = 12;
    for (int i = 0; i < 60; i++) {
        pkt[offset++] = 5;
        memcpy(pkt + offset, "aaaaa", 5);
        offset += 5;
    }
    pkt[offset++] = 0;
    pkt[offset++] = 0;
    pkt[offset++] = 1; /* A */
    pkt[offset++] = 0;
    pkt[offset++] = 1; /* IN */

    memset(pkt, 0, 4);
    pkt[4] = 0;
    pkt[5] = 1; /* QDCOUNT = 1 */

    TEST_ASSERT_EQUAL(-1, dns_build_response(pkt, offset, &ip));
}
