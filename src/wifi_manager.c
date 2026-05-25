#include "wifi_manager.h"
#include "ap_provisioning.h"
#include "nvs_storage.h"
#include "config.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include <stdatomic.h>
#include <string.h>

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define AP_FALLBACK_RETRIES 5
#define MAX_RETRY 10

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static _Atomic bool s_connected = false;
static uint32_t s_backoff_ms = WIFI_BACKOFF_INITIAL_MS;
static TimerHandle_t s_reconnect_timer = NULL;
static wifi_ap_fallback_cb_t s_fallback_cb = NULL;
static bool s_fallback_mode = false;  /* true = limited retries for initial connect */

/* AP+STA mode state */
static esp_netif_t *s_ap_netif = NULL;
static TimerHandle_t s_sta_retry_timer = NULL;
static TimerHandle_t s_ap_auto_off_timer = NULL;
static bool s_apsta_active = false;

static uint32_t calc_backoff_with_jitter(void)
{
    uint32_t base = s_backoff_ms;
    if (s_backoff_ms < WIFI_BACKOFF_MAX_MS) {
        s_backoff_ms *= 2;
        if (s_backoff_ms > WIFI_BACKOFF_MAX_MS)
            s_backoff_ms = WIFI_BACKOFF_MAX_MS;
    }
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
        s_retry_num++;

        if (s_fallback_mode && s_retry_num >= AP_FALLBACK_RETRIES) {
            /* Trigger AP fallback after AP_FALLBACK_RETRIES failed attempts */
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "WiFi failed %d times, triggering AP fallback", s_retry_num);
            if (s_fallback_cb) {
                s_fallback_cb();
            }
            return;
        }
        uint32_t delay = calc_backoff_with_jitter();
        ESP_LOGI(TAG, "Retrying WiFi in %lu ms (%d/%d)",
                 (unsigned long)delay, s_retry_num,
                 s_fallback_mode ? AP_FALLBACK_RETRIES : MAX_RETRY);

        if (!s_fallback_mode && s_retry_num >= MAX_RETRY) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            s_retry_num = 0;
        }

        xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(delay), 0);
        xTimerStart(s_reconnect_timer, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_backoff_ms = WIFI_BACKOFF_INITIAL_MS;
        s_connected = true;
        s_fallback_mode = false;  /* Connected, disable fallback limit */
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        /* In APSTA mode, start AP auto-off timer (2 min) */
        if (s_ap_auto_off_timer) {
            xTimerStart(s_ap_auto_off_timer, 0);
        }
    }
}

static esp_err_t wifi_start_sta(void)
{
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

    ESP_LOGI(TAG, "WiFi STA started, connecting to '%s'", ssid);
    return ESP_OK;
}

static esp_err_t wifi_common_init(void)
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

    return ESP_OK;
}

esp_err_t wifi_manager_init(void)
{
    s_fallback_cb = NULL;
    s_fallback_mode = false;

    wifi_common_init();
    wifi_start_sta();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_manager_init_with_fallback(wifi_ap_fallback_cb_t cb)
{
    s_fallback_cb = cb;
    s_fallback_mode = true;
    s_retry_num = 0;
    s_backoff_ms = WIFI_BACKOFF_INITIAL_MS;

    wifi_common_init();
    wifi_start_sta();

    /* Wait for connect or fallback trigger.
     * Backoff sum for 5 retries: ~5+10+20+40+60 = 135s + jitter.
     * Use 180s to be safe. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(180000));

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

void wifi_manager_stop_sta(void)
{
    if (s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi STA stopped");
}

void wifi_manager_restart_sta(void)
{
    s_retry_num = 0;
    s_backoff_ms = WIFI_BACKOFF_INITIAL_MS;
    s_fallback_mode = true;  /* Re-enable fallback for fresh attempt */
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    wifi_start_sta();
    ESP_LOGI(TAG, "WiFi STA restarted");
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

static void apsta_retry_cb(TimerHandle_t timer)
{
    (void)timer;
    if (!s_connected) {
        ESP_LOGI(TAG, "AP+STA: background STA retry");
        esp_wifi_connect();
    }
}

static void ap_auto_off_cb(TimerHandle_t timer)
{
    (void)timer;
    if (s_connected) {
        ESP_LOGI(TAG, "AP+STA: STA connected, stopping AP after 2min");
        ap_provisioning_stop();
        wifi_manager_stop_ap();
    }
}

esp_err_t wifi_manager_start_apsta(void)
{
    /* Get MAC for AP SSID/password */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32], ap_pass[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "SchoolBell_%02X%02X", mac[4], mac[5]);
    snprintf(ap_pass, sizeof(ap_pass), "SchoolBell_%02X%02X%02X", mac[3], mac[4], mac[5]);

    /* Read STA credentials from NVS */
    char ssid[33] = {0}, pass[65] = {0};
    if (nvs_storage_get_str(NVS_NAMESPACE_WIFI, "ssid", ssid, sizeof(ssid)) != ESP_OK
        || strlen(ssid) == 0) {
        ssid[0] = '\0'; /* No STA creds — AP only effectively */
    } else {
        nvs_storage_get_str(NVS_NAMESPACE_WIFI, "pass", pass, sizeof(pass));
    }

    /* Create AP netif if not already */
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* Switch to APSTA mode */
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Configure AP */
    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(ap_config.ap.ssid, ap_ssid, strlen(ap_ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    memcpy(ap_config.ap.password, ap_pass, strlen(ap_pass));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    /* Configure STA if credentials exist */
    if (strlen(ssid) > 0) {
        wifi_config_t sta_config = {
            .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK },
        };
        memcpy(sta_config.sta.ssid, ssid, strlen(ssid));
        memcpy(sta_config.sta.password, pass, strlen(pass));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    if (strlen(ssid) > 0) {
        esp_wifi_connect();
    }

    ESP_LOGI(TAG, "AP+STA started: AP='%s', STA='%s'", ap_ssid, ssid);

    /* Background STA retry timer (30s) */
    if (!s_sta_retry_timer) {
        s_sta_retry_timer = xTimerCreate("apsta_retry", pdMS_TO_TICKS(30000),
                                         pdTRUE, NULL, apsta_retry_cb);
    }
    xTimerStart(s_sta_retry_timer, 0);

    /* AP auto-off timer (2 min after STA connects) — started when STA connects */
    if (!s_ap_auto_off_timer) {
        s_ap_auto_off_timer = xTimerCreate("ap_off", pdMS_TO_TICKS(120000),
                                           pdFALSE, NULL, ap_auto_off_cb);
    }

    s_apsta_active = true;
    return ESP_OK;
}

void wifi_manager_stop_ap(void)
{
    if (s_sta_retry_timer) {
        xTimerStop(s_sta_retry_timer, 0);
    }
    if (s_ap_auto_off_timer) {
        xTimerStop(s_ap_auto_off_timer, 0);
    }

    /* Switch from APSTA to STA only */
    esp_wifi_set_mode(WIFI_MODE_STA);

    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    s_apsta_active = false;
    ESP_LOGI(TAG, "AP stopped, STA-only mode");
}

bool wifi_manager_is_ap_active(void)
{
    return s_apsta_active;
}
