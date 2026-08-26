#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "dns_packet.h"

static const char *TAG = "dns_packet";

typedef enum {
    DNS_PARSE_OK = 0,
    DNS_PARSE_MALFORMED,   /* drop silently */
    DNS_PARSE_UNSUPPORTED, /* answer NOTIMP */
} dns_parse_result_t;

enum {
    DNS_RCODE_NOERROR = 0,
    DNS_RCODE_NXDOMAIN = 3,
    DNS_RCODE_NOTIMP = 4,
};

enum {
    DNS_TYPE_A = 1,
    DNS_CLASS_IN = 1,
};

typedef struct {
    uint16_t qtype;
    uint16_t qclass;
    int question_end;
} dns_question_t;

static uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void write_u16_be(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xff);
}

static uint8_t ascii_lower(uint8_t character)
{
    return character >= 'A' && character <= 'Z' ? character + ('a' - 'A')
                                                : character;
}

static dns_parse_result_t dns_parse_header(const uint8_t *packet, int len)
{
    if (len < DNS_HEADER_LEN) return DNS_PARSE_MALFORMED;
    if ((packet[2] & 0x80) != 0) return DNS_PARSE_MALFORMED; /* QR=1: response */
    if ((packet[2] & 0x78) != 0) return DNS_PARSE_UNSUPPORTED; /* OPCODE != QUERY */
    if (read_u16_be(&packet[4]) != 1) return DNS_PARSE_UNSUPPORTED; /* QDCOUNT */
    return DNS_PARSE_OK;
}

static dns_parse_result_t dns_parse_question(const uint8_t *packet, int len,
                                             dns_question_t *out)
{
    int offset = DNS_HEADER_LEN;
    for (;;) {
        if (offset >= len) return DNS_PARSE_MALFORMED;
        uint8_t label_length = packet[offset++];
        if (label_length == 0) break;
        if ((label_length & 0xc0) != 0 || label_length > 63 ||
            offset + label_length > len) {
            return DNS_PARSE_MALFORMED;
        }
        offset += label_length;
    }
    if (offset + 4 > len) return DNS_PARSE_MALFORMED;
    out->qtype = read_u16_be(&packet[offset]);
    out->qclass = read_u16_be(&packet[offset + 2]);
    out->question_end = offset + 4;
    return DNS_PARSE_OK;
}

static bool question_name_equals(const uint8_t *packet, int packet_len,
                                 const char *expected_name)
{
    int offset = DNS_HEADER_LEN;
    const char *expected = expected_name;
    while (offset < packet_len) {
        uint8_t label_length = packet[offset++];
        if (label_length == 0) return *expected == '\0';
        if ((label_length & 0xc0) != 0 || label_length > 63 ||
            offset + label_length > packet_len) {
            return false;
        }
        for (uint8_t i = 0; i < label_length; i++) {
            if (expected[i] == '\0' || expected[i] == '.' ||
                ascii_lower(packet[offset + i]) !=
                    ascii_lower((uint8_t)expected[i])) {
                return false;
            }
        }
        expected += label_length;
        if (*expected == '.') {
            expected++;
        } else if (*expected != '\0') {
            return false;
        }
        offset += label_length;
    }
    return false;
}

static bool is_adguard_injection_domain(const uint8_t *packet, int packet_len)
{
    return question_name_equals(packet, packet_len, "local.adguard.org") ||
           question_name_equals(packet, packet_len, "local.adguard.com");
}

/*
 * Builds a response in-place. Response flags always advertise no
 * recursion: QR=1, OPCODE=0, AA=0, TC=0, RD copied from request,
 * RA=0, Z=0.
 */
int dns_build_response(uint8_t *packet, int query_len,
                       const esp_ip4_addr_t *redirect_ip)
{
    dns_parse_result_t parse = dns_parse_header(packet, query_len);
    if (parse == DNS_PARSE_MALFORMED) return -1;

    uint8_t rd = packet[2] & 0x01;
    dns_question_t question = {0};
    bool have_question = false;
    if (parse == DNS_PARSE_OK) {
        dns_parse_result_t result =
            dns_parse_question(packet, query_len, &question);
        if (result == DNS_PARSE_MALFORMED) return -1;
        if (result == DNS_PARSE_UNSUPPORTED) parse = DNS_PARSE_UNSUPPORTED;
        have_question = true;
    }

    uint8_t rcode = parse == DNS_PARSE_UNSUPPORTED ? DNS_RCODE_NOTIMP
                                                   : DNS_RCODE_NOERROR;
    packet[2] = (uint8_t)(0x80 | rd); /* QR=1, OPCODE=0, RD copied */
    packet[3] = rcode;                /* AA=0, TC=0, RA=0, Z=0, RCODE */
    write_u16_be(&packet[6], 0);      /* ANCOUNT */
    write_u16_be(&packet[8], 0);      /* NSCOUNT */
    write_u16_be(&packet[10], 0);     /* ARCOUNT */

    if (rcode != DNS_RCODE_NOERROR || !have_question)
        return question.question_end > 0 ? question.question_end
                                         : DNS_HEADER_LEN;

    if (question.qclass != DNS_CLASS_IN) {
        packet[3] = DNS_RCODE_NOTIMP;
        return question.question_end;
    }

    if (is_adguard_injection_domain(packet, query_len)) {
        ESP_LOGI(TAG, "Rejected AdGuard local content-script DNS query");
        packet[3] = DNS_RCODE_NXDOMAIN;
        return question.question_end;
    }

    if (question.qtype != DNS_TYPE_A)
        return question.question_end; /* AAAA and others: NOERROR, 0 answers */

    if (question.question_end + DNS_ANSWER_LEN > DNS_MAX_PACKET_LEN)
        return -1;

    write_u16_be(&packet[6], 1); /* ANCOUNT = 1 */
    int offset = question.question_end;
    packet[offset++] = 0xc0;
    packet[offset++] = 0x0c; /* compressed name points to QNAME */
    write_u16_be(&packet[offset], DNS_TYPE_A);
    offset += 2;
    write_u16_be(&packet[offset], DNS_CLASS_IN);
    offset += 2;
    write_u16_be(&packet[offset], 0);
    offset += 2;
    write_u16_be(&packet[offset], DNS_ANSWER_TTL_S);
    offset += 2;
    write_u16_be(&packet[offset], 4);
    offset += 2;
    memcpy(packet + offset, &redirect_ip->addr, sizeof(redirect_ip->addr));
    offset += 4;
    return offset;
}
