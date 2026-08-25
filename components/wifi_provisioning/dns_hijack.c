#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "dns_hijack.h"

static const char *TAG = "dns_hijack";

#define DNS_PORT 53
#define DNS_MAX_PACKET_LEN 512
#define DNS_HEADER_LEN 12
#define DNS_ANSWER_LEN 16
#define DNS_TASK_STACK_SIZE 4096
#define SOFTAP_IP "192.168.4.1"

static TaskHandle_t s_dns_task;
static volatile bool s_running;
static volatile int s_dns_socket = -1;

static int question_end_offset(const uint8_t *query, int query_len)
{
    if (query_len < DNS_HEADER_LEN || query[4] == 0 || query[5] == 0) return -1;
    int offset = DNS_HEADER_LEN;
    while (offset < query_len) {
        uint8_t label_length = query[offset++];
        if (label_length == 0) break;
        if ((label_length & 0xc0) != 0 || label_length > 63 ||
            offset + label_length > query_len) {
            return -1;
        }
        offset += label_length;
    }
    return offset + 4 <= query_len ? offset + 4 : -1;
}

static uint8_t ascii_lower(uint8_t character)
{
    return character >= 'A' && character <= 'Z' ? character + ('a' - 'A')
                                                 : character;
}

static bool question_name_equals(const uint8_t *query, int query_len,
                                 const char *expected_name)
{
    int offset = DNS_HEADER_LEN;
    const char *expected = expected_name;
    while (offset < query_len) {
        uint8_t label_length = query[offset++];
        if (label_length == 0) return *expected == '\0';
        if ((label_length & 0xc0) != 0 || label_length > 63 ||
            offset + label_length > query_len) {
            return false;
        }
        for (uint8_t i = 0; i < label_length; i++) {
            if (expected[i] == '\0' || expected[i] == '.' ||
                ascii_lower(query[offset + i]) !=
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

static bool is_adguard_injection_domain(const uint8_t *query, int query_len)
{
    return question_name_equals(query, query_len, "local.adguard.org") ||
           question_name_equals(query, query_len, "local.adguard.com");
}

static int build_dns_response(uint8_t packet[DNS_MAX_PACKET_LEN], int query_len)
{
    int question_end = question_end_offset(packet, query_len);
    if (question_end < 0) return -1;

    packet[2] = 0x81; /* response, recursion desired */
    packet[3] = 0x80; /* recursion available, no error */
    packet[4] = 0;
    packet[5] = 1;    /* one question */
    packet[6] = 0;
    packet[7] = 0;
    packet[8] = packet[9] = packet[10] = packet[11] = 0;

    if (is_adguard_injection_domain(packet, query_len)) {
        packet[3] = 0x83; /* recursion available, NXDOMAIN */
        ESP_LOGI(TAG, "Rejected AdGuard local content-script DNS query");
        return question_end;
    }
    if (question_end + DNS_ANSWER_LEN > DNS_MAX_PACKET_LEN) return -1;
    packet[7] = 1; /* one answer */

    int offset = question_end;
    packet[offset++] = 0xc0;
    packet[offset++] = 0x0c; /* compressed name points to QNAME */
    packet[offset++] = 0;
    packet[offset++] = 1;    /* A */
    packet[offset++] = 0;
    packet[offset++] = 1;    /* IN */
    packet[offset++] = 0;
    packet[offset++] = 0;
    packet[offset++] = 0;
    packet[offset++] = 30;   /* TTL */
    packet[offset++] = 0;
    packet[offset++] = 4;
    const uint8_t ip[] = {192, 168, 4, 1};
    memcpy(packet + offset, ip, sizeof(ip));
    return offset + sizeof(ip);
}

static void dns_task(void *arg)
{
    uint8_t packet[DNS_MAX_PACKET_LEN];
    struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = inet_addr(SOFTAP_IP),
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    s_dns_socket = sock;
    if (sock < 0 || bind(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        ESP_LOGE(TAG, "Could not bind DNS server to %s:%d", SOFTAP_IP, DNS_PORT);
        if (sock >= 0) close(sock);
        s_dns_socket = -1;
        s_running = false;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ESP_LOGI(TAG, "Captive DNS listening on %s:%d", SOFTAP_IP, DNS_PORT);

    while (s_running) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int query_len = recvfrom(sock, packet, sizeof(packet), 0,
                                 (struct sockaddr *)&client, &client_len);
        if (query_len <= 0) continue;
        int response_len = build_dns_response(packet, query_len);
        if (response_len > 0) {
            sendto(sock, packet, response_len, 0,
                   (struct sockaddr *)&client, client_len);
        }
    }

    close(sock);
    s_dns_socket = -1;
    s_dns_task = NULL;
    ESP_LOGI(TAG, "Captive DNS stopped");
    vTaskDelete(NULL);
}

int dns_hijack_start(void)
{
    if (s_running) return 0;
    s_running = true;
    if (xTaskCreate(dns_task, "dns_hijack", DNS_TASK_STACK_SIZE,
                    NULL, 5, &s_dns_task) != pdPASS) {
        s_running = false;
        s_dns_task = NULL;
        return -1;
    }
    return 0;
}

void dns_hijack_stop(void)
{
    s_running = false;
    int sock = s_dns_socket;
    if (sock >= 0) shutdown(sock, SHUT_RDWR);
}
