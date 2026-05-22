#include "wifi_manager.h"
#include "nvs_storage.h"
#include "config.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include <stdatomic.h>
#include <string.h>

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          10

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static _Atomic bool s_connected = false;
static uint32_t s_backoff_ms = WIFI_BACKOFF_INITIAL_MS;
static TimerHandle_t s_reconnect_timer = NULL;

static uint32_t calc_backoff_with_jitter(void)
{
    uint32_t base = s_backoff_ms;
    if (s_backoff_ms < WIFI_BACKOFF_MAX_MS) {
        s_backoff_ms *= 2;
        if (s_backoff_ms > WIFI_BACKOFF_MAX_MS)
            s_backoff_ms = WIFI_BACKOFF_MAX_MS;
    }
    /* Add random jitter: 0 to +50% (avoid underflow from subtraction) */
    uint32_t jitter = esp_random() % (base / 2 + 1);
    return base + jitter;
}

static void reconnect_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    esp_wifi_connect();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        uint32_t delay;
        if (s_retry_num < MAX_RETRY) {
            delay = calc_backoff_with_jitter();
            ESP_LOGI(TAG, "Retrying WiFi in %lu ms (%d/%d)",
                     (unsigned long)delay, s_retry_num + 1, MAX_RETRY);
            s_retry_num++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi failed after %d retries, backing off", MAX_RETRY);
            s_retry_num = 0;
            delay = calc_backoff_with_jitter();
        }
        /* Non-blocking: schedule reconnect via timer instead of vTaskDelay */
        xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(delay), 0);
        xTimerStart(s_reconnect_timer, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_backoff_ms = WIFI_BACKOFF_INITIAL_MS; /* Reset backoff on success */
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_reconnect_timer = xTimerCreate("wifi_reconn", pdMS_TO_TICKS(5000),
                                     pdFALSE, NULL, reconnect_timer_cb);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};

    if (nvs_storage_get_str(NVS_NAMESPACE_WIFI, "ssid", ssid, sizeof(ssid)) != ESP_OK
        || strlen(ssid) == 0) {
        strncpy(ssid, DEFAULT_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, DEFAULT_WIFI_PASS, sizeof(pass) - 1);
        ESP_LOGW(TAG, "Using default WiFi credentials");
    } else {
        nvs_storage_get_str(NVS_NAMESPACE_WIFI, "pass", pass, sizeof(pass));
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi_config.sta.password, pass, strlen(pass));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init complete, connecting to '%s'", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

int8_t wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}
