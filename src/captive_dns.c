#include "captive_dns.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "captive_dns";

#define DNS_PORT 53
#define DNS_BUF_SIZE 512

static TaskHandle_t s_dns_task = NULL;
static int s_sock = -1;
static volatile bool s_dns_running = false;

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[DNS_BUF_SIZE];
    struct sockaddr_in client;
    socklen_t client_len;

    while (s_dns_running) {
        client_len = sizeof(client);
        int len = recvfrom(s_sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;  /* Minimum DNS header or socket closed */

        /* Build response: copy query, set response flags, append answer */
        uint8_t resp[DNS_BUF_SIZE];
        memcpy(resp, buf, len);

        /* Set QR=1 (response), AA=1, RCODE=0 */
        resp[2] = 0x84;
        resp[3] = 0x00;
        /* ANCOUNT = 1 */
        resp[6] = 0x00;
        resp[7] = 0x01;

        /* Append answer: pointer to question name + A record */
        int pos = len;
        resp[pos++] = 0xC0;  /* Name pointer */
        resp[pos++] = 0x0C;  /* Offset to question name */
        resp[pos++] = 0x00; resp[pos++] = 0x01;  /* Type A */
        resp[pos++] = 0x00; resp[pos++] = 0x01;  /* Class IN */
        resp[pos++] = 0x00; resp[pos++] = 0x00;
        resp[pos++] = 0x00; resp[pos++] = 0x0A;  /* TTL = 10s */
        resp[pos++] = 0x00; resp[pos++] = 0x04;  /* RDLENGTH = 4 */
        /* 192.168.4.1 */
        resp[pos++] = 192;
        resp[pos++] = 168;
        resp[pos++] = 4;
        resp[pos++] = 1;

        sendto(s_sock, resp, pos, 0,
               (struct sockaddr *)&client, client_len);
    }

    /* Clean exit */
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(void)
{
    if (s_dns_running) return ESP_OK;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    /* Set receive timeout so task can check s_dns_running periodically */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_dns_running = true;
    xTaskCreate(dns_task, "captive_dns", 2048, NULL, 5, &s_dns_task);
    ESP_LOGI(TAG, "Captive DNS started");
    return ESP_OK;
}

void captive_dns_stop(void)
{
    if (!s_dns_running) return;

    s_dns_running = false;

    /* Close socket to unblock recvfrom() */
    if (s_sock >= 0) {
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
        s_sock = -1;
    }

    /* Wait for task to self-delete */
    vTaskDelay(pdMS_TO_TICKS(100));
    s_dns_task = NULL;

    ESP_LOGI(TAG, "Captive DNS stopped");
}
