#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "dns_hijack.h"
#include "dns_packet.h"

#define DNS_PORT 53
#define DNS_TASK_STACK_SIZE 3072
#define DNS_TASK_PRIORITY 5
#define DNS_START_TIMEOUT_MS 2000
#define DNS_STOP_TIMEOUT_MS 2000

static const char *TAG = "dns_hijack";

typedef enum {
    DNS_STATE_STOPPED = 0,
    DNS_STATE_STARTING,
    DNS_STATE_RUNNING,
    DNS_STATE_STOPPING,
} dns_state_t;

#define DNS_EVT_RUNNING BIT0
#define DNS_EVT_STOPPED BIT1
#define DNS_EVT_START_FAILED BIT2

static portMUX_TYPE s_dns_init_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_dns_mutex;
static EventGroupHandle_t s_dns_events;
static bool s_dns_sync_ready;

/* Protected by s_dns_mutex. */
static dns_state_t s_dns_state = DNS_STATE_STOPPED;
static TaskHandle_t s_dns_task;
static int s_dns_socket = -1;
static esp_ip4_addr_t s_redirect_ip;

static esp_err_t dns_ensure_sync_primitives(void)
{
    bool ready;

    portENTER_CRITICAL(&s_dns_init_lock);
    ready = s_dns_sync_ready;
    portEXIT_CRITICAL(&s_dns_init_lock);
    if (ready) return ESP_OK;

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    EventGroupHandle_t events = xEventGroupCreate();
    if (mutex == NULL || events == NULL) {
        if (mutex != NULL) vSemaphoreDelete(mutex);
        if (events != NULL) vEventGroupDelete(events);
        return ESP_ERR_NO_MEM;
    }

    bool adopted = false;
    portENTER_CRITICAL(&s_dns_init_lock);
    if (!s_dns_sync_ready) {
        s_dns_mutex = mutex;
        s_dns_events = events;
        s_dns_sync_ready = true;
        adopted = true;
    }
    portEXIT_CRITICAL(&s_dns_init_lock);

    if (!adopted) {
        vSemaphoreDelete(mutex);
        vEventGroupDelete(events);
    }
    return ESP_OK;
}

/* Single cleanup path for the DNS task. */
static void dns_task_cleanup(int sock, bool set_start_failed)
{
    if (sock >= 0) close(sock);

    xSemaphoreTake(s_dns_mutex, portMAX_DELAY);
    s_dns_socket = -1;
    s_dns_task = NULL;
    s_dns_state = DNS_STATE_STOPPED;
    xSemaphoreGive(s_dns_mutex);

    EventBits_t bits = DNS_EVT_STOPPED;
    if (set_start_failed) bits |= DNS_EVT_START_FAILED;
    xEventGroupSetBits(s_dns_events, bits);
    vTaskDelete(NULL);
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t packet[DNS_MAX_PACKET_LEN];
    struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = s_redirect_ip.addr,
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bool bound = false;
    if (sock >= 0) {
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        bound = bind(sock, (struct sockaddr *)&server, sizeof(server)) == 0;
    }

    if (!bound) {
        ESP_LOGE(TAG, "Could not bind DNS server to " IPSTR ":%d",
                 IP2STR(&s_redirect_ip), DNS_PORT);
        dns_task_cleanup(sock, true);
        return;
    }

    xEventGroupSetBits(s_dns_events, DNS_EVT_RUNNING);
    ESP_LOGI(TAG, "Captive DNS listening on " IPSTR ":%d",
             IP2STR(&s_redirect_ip), DNS_PORT);

    for (;;) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int query_len = recvfrom(sock, packet, sizeof(packet), 0,
                                 (struct sockaddr *)&client, &client_len);
        if (query_len <= 0) {
            xSemaphoreTake(s_dns_mutex, portMAX_DELAY);
            bool stopping = s_dns_state == DNS_STATE_STOPPING;
            xSemaphoreGive(s_dns_mutex);
            if (stopping) break;
            continue; /* receive timeout */
        }

        int response_len = dns_build_response(packet, query_len, &s_redirect_ip);
        if (response_len > 0) {
            sendto(sock, packet, response_len, 0,
                   (struct sockaddr *)&client, client_len);
        }
    }

    ESP_LOGI(TAG, "Captive DNS stopped");
    dns_task_cleanup(sock, false);
}

esp_err_t dns_hijack_start(const esp_ip4_addr_t *redirect_ip)
{
    if (redirect_ip == NULL || redirect_ip->addr == 0)
        return ESP_ERR_INVALID_ARG;

    esp_err_t error = dns_ensure_sync_primitives();
    if (error != ESP_OK) return error;

    for (int attempt = 0; attempt < 10; attempt++) {
        xSemaphoreTake(s_dns_mutex, portMAX_DELAY);
        switch (s_dns_state) {
        case DNS_STATE_RUNNING: {
            if (s_redirect_ip.addr == redirect_ip->addr) {
                xSemaphoreGive(s_dns_mutex);
                return ESP_OK;
            }
            /* Different IP requested: restart with the new address. */
            s_dns_state = DNS_STATE_STOPPING;
            int sock = s_dns_socket;
            xSemaphoreGive(s_dns_mutex);
            if (sock >= 0) shutdown(sock, SHUT_RDWR);
            xEventGroupWaitBits(s_dns_events, DNS_EVT_STOPPED, pdFALSE,
                                pdFALSE, pdMS_TO_TICKS(DNS_STOP_TIMEOUT_MS));
            break;
        }

        case DNS_STATE_STARTING:
            xSemaphoreGive(s_dns_mutex);
            xEventGroupWaitBits(
                s_dns_events,
                DNS_EVT_RUNNING | DNS_EVT_START_FAILED | DNS_EVT_STOPPED,
                pdFALSE, pdFALSE, pdMS_TO_TICKS(DNS_START_TIMEOUT_MS));
            break;

        case DNS_STATE_STOPPING:
            xSemaphoreGive(s_dns_mutex);
            {
                EventBits_t stopped = xEventGroupWaitBits(
                    s_dns_events, DNS_EVT_STOPPED, pdFALSE, pdFALSE,
                    pdMS_TO_TICKS(DNS_STOP_TIMEOUT_MS));
                if ((stopped & DNS_EVT_STOPPED) == 0)
                    return ESP_ERR_INVALID_STATE;
            }
            break;

        case DNS_STATE_STOPPED:
        default:
            s_redirect_ip = *redirect_ip;
            s_dns_state = DNS_STATE_STARTING;
            xEventGroupClearBits(s_dns_events,
                                 DNS_EVT_RUNNING | DNS_EVT_STOPPED |
                                     DNS_EVT_START_FAILED);
            if (xTaskCreate(dns_task, "dns_hijack", DNS_TASK_STACK_SIZE, NULL,
                            DNS_TASK_PRIORITY, &s_dns_task) != pdPASS) {
                s_dns_task = NULL;
                s_dns_state = DNS_STATE_STOPPED;
                xSemaphoreGive(s_dns_mutex);
                ESP_LOGE(TAG, "Could not create captive DNS task");
                return ESP_ERR_NO_MEM;
            }
            xSemaphoreGive(s_dns_mutex);

            EventBits_t bits = xEventGroupWaitBits(
                s_dns_events, DNS_EVT_RUNNING | DNS_EVT_START_FAILED, pdFALSE,
                pdFALSE, pdMS_TO_TICKS(DNS_START_TIMEOUT_MS));
            if ((bits & DNS_EVT_RUNNING) != 0) return ESP_OK;
            if ((bits & DNS_EVT_START_FAILED) != 0)
                return ESP_ERR_INVALID_STATE;
            ESP_LOGE(TAG, "Timed out waiting for captive DNS start");
            return ESP_ERR_TIMEOUT;
        }
    }

    ESP_LOGE(TAG, "Captive DNS did not reach RUNNING state");
    return ESP_ERR_INVALID_STATE;
}

esp_err_t dns_hijack_stop(void)
{
    if (!s_dns_sync_ready) return ESP_OK;

    xSemaphoreTake(s_dns_mutex, portMAX_DELAY);
    if (s_dns_state == DNS_STATE_STOPPED) {
        xSemaphoreGive(s_dns_mutex);
        return ESP_OK;
    }
    s_dns_state = DNS_STATE_STOPPING;
    int sock = s_dns_socket;
    xSemaphoreGive(s_dns_mutex);

    if (sock >= 0) shutdown(sock, SHUT_RDWR);

    EventBits_t bits =
        xEventGroupWaitBits(s_dns_events, DNS_EVT_STOPPED, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(DNS_STOP_TIMEOUT_MS));
    if ((bits & DNS_EVT_STOPPED) == 0) {
        /* Lifecycle stays STOPPING; new starts stay rejected until the old
         * task signals DNS_EVT_STOPPED. */
        ESP_LOGE(TAG, "Timed out stopping captive DNS");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool dns_hijack_is_running(void)
{
    if (!s_dns_sync_ready) return false;

    xSemaphoreTake(s_dns_mutex, portMAX_DELAY);
    bool running = s_dns_state == DNS_STATE_RUNNING;
    xSemaphoreGive(s_dns_mutex);
    return running;
}
