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

static int build_dns_response(const uint8_t *query, int query_len,
                              uint8_t response[DNS_MAX_PACKET_LEN])
{
    int question_end = question_end_offset(query, query_len);
    if (question_end < 0 || question_end + DNS_ANSWER_LEN > DNS_MAX_PACKET_LEN) return -1;

    memcpy(response, query, question_end);
    response[2] = 0x81; /* response, recursion desired */
    response[3] = 0x80; /* recursion available, no error */
    response[6] = 0;
    response[7] = 1;    /* one answer */
    response[8] = response[9] = response[10] = response[11] = 0;

    int offset = question_end;
    response[offset++] = 0xc0;
    response[offset++] = 0x0c; /* compressed name points to QNAME */
    response[offset++] = 0;
    response[offset++] = 1;    /* A */
    response[offset++] = 0;
    response[offset++] = 1;    /* IN */
    response[offset++] = 0;
    response[offset++] = 0;
    response[offset++] = 0;
    response[offset++] = 30;   /* TTL */
    response[offset++] = 0;
    response[offset++] = 4;
    const uint8_t ip[] = {192, 168, 4, 1};
    memcpy(response + offset, ip, sizeof(ip));
    return offset + sizeof(ip);
}

static void dns_task(void *arg)
{
    uint8_t query[DNS_MAX_PACKET_LEN];
    uint8_t response[DNS_MAX_PACKET_LEN];
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
        int query_len = recvfrom(sock, query, sizeof(query), 0,
                                 (struct sockaddr *)&client, &client_len);
        if (query_len <= 0) continue;
        int response_len = build_dns_response(query, query_len, response);
        if (response_len > 0) {
            sendto(sock, response, response_len, 0,
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
    if (xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 5, &s_dns_task) != pdPASS) {
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
