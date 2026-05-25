#include "ap_provisioning.h"
#include "captive_dns.h"
#include "wifi_manager.h"
#include "nvs_storage.h"
#include "config.h"

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ap_prov";

#define AP_TIMEOUT_MS 300000  /* 5 minutes */
#define BRUTEFORCE_MAX_ATTEMPTS 3
#define BRUTEFORCE_LOCKOUT_MS   30000  /* 30 seconds */

static httpd_handle_t s_httpd = NULL;
static TimerHandle_t s_timeout_timer = NULL;
static bool s_active = false;
static bool s_portal_only = false;  /* true when started via start_portal_only */
static esp_netif_t *s_ap_netif = NULL;

/* Brute-force protection */
static int s_failed_attempts = 0;
static TickType_t s_lockout_start = 0;
static bool s_locked_out = false;

static const char HTML_PAGE[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>SchoolBell WiFi</title>"
    "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:20px}"
    "input{width:100%;padding:10px;margin:8px 0;box-sizing:border-box;font-size:16px}"
    "button{width:100%;padding:12px;background:#2196F3;color:#fff;border:none;font-size:18px;cursor:pointer}"
    "h2{text-align:center}</style></head><body>"
    "<h2>WiFi Sozlash</h2>"
    "<form method='POST' action='/save'>"
    "<label>WiFi nomi (SSID):</label><input name='ssid' maxlength='32' required>"
    "<label>Parol:</label><input name='pass' type='password' minlength='8' maxlength='64' required>"
    "<button type='submit'>Saqlash</button>"
    "</form></body></html>";

static const char HTML_SUCCESS[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saqlandi</title></head><body style='font-family:sans-serif;text-align:center;padding:40px'>"
    "<h2>Saqlandi!</h2><p>Qurilma qayta yuklanmoqda...</p></body></html>";

static const char HTML_ERROR[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Xato</title></head><body style='font-family:sans-serif;text-align:center;padding:40px'>"
    "<h2>Xato!</h2><p>SSID 1-32, parol 8-64 belgi bo'lishi kerak.</p>"
    "<a href='/'>Qaytish</a></body></html>";

static const char HTML_LOCKED[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Bloklangan</title></head><body style='font-family:sans-serif;text-align:center;padding:40px'>"
    "<h2>Bloklangan</h2><p>Juda ko'p noto'g'ri urinish. 30 soniya kuting.</p>"
    "</body></html>";

static void set_security_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t handler_get_root(httpd_req_t *req)
{
    set_security_headers(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_redirect(httpd_req_t *req)
{
    set_security_headers(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* Simple URL-decode in place */
static void url_decode(char *str)
{
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            char ch = (char)strtol(hex, NULL, 16);
            if (ch == '\0') { src += 3; continue; } /* Skip NUL bytes */
            *dst++ = ch;
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Strip control characters (0x00-0x1F, 0x7F) from string */
static void sanitize_input(char *str)
{
    char *src = str, *dst = str;
    while (*src) {
        if ((unsigned char)*src >= 0x20 && (unsigned char)*src != 0x7F) {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
}

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t handler_post_save(httpd_req_t *req)
{
    /* Brute-force lockout check */
    if (s_locked_out) {
        TickType_t elapsed = xTaskGetTickCount() - s_lockout_start;
        if (elapsed < pdMS_TO_TICKS(BRUTEFORCE_LOCKOUT_MS)) {
            set_security_headers(req);
            httpd_resp_set_type(req, "text/html");
            return httpd_resp_send(req, HTML_LOCKED, HTTPD_RESP_USE_STRLEN);
        }
        s_locked_out = false;
        s_failed_attempts = 0;
    }

    char buf[200] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }

    /* Parse ssid=...&pass=... */
    char ssid[33] = {0}, pass[65] = {0};
    char *s = strstr(buf, "ssid=");
    char *p = strstr(buf, "pass=");

    if (s) {
        s += 5;
        char *end = strchr(s, '&');
        if (end) *end = '\0';
        strncpy(ssid, s, sizeof(ssid) - 1);
        if (end) *end = '&';
        url_decode(ssid);
        sanitize_input(ssid);
    }
    if (p) {
        p += 5;
        char *end = strchr(p, '&');
        if (end) *end = '\0';
        strncpy(pass, p, sizeof(pass) - 1);
        url_decode(pass);
        sanitize_input(pass);
    }

    /* Validate */
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);
    if (ssid_len < 1 || ssid_len > 32 || pass_len < 8 || pass_len > 64) {
        s_failed_attempts++;
        if (s_failed_attempts >= BRUTEFORCE_MAX_ATTEMPTS) {
            s_locked_out = true;
            s_lockout_start = xTaskGetTickCount();
            ESP_LOGW(TAG, "Brute-force lockout triggered (%d attempts)", s_failed_attempts);
        }
        set_security_headers(req);
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, HTML_ERROR, HTTPD_RESP_USE_STRLEN);
    }

    /* Valid submission — reset brute-force counter */
    s_failed_attempts = 0;

    /* Save to NVS */
    nvs_storage_set_str(NVS_NAMESPACE_WIFI, "ssid", ssid);
    nvs_storage_set_str(NVS_NAMESPACE_WIFI, "pass", pass);
    ESP_LOGI(TAG, "WiFi credentials saved: SSID='%s'", ssid);

    set_security_headers(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_SUCCESS, HTTPD_RESP_USE_STRLEN);

    /* Deferred reboot via one-shot timer so HTTP response is flushed first */
    esp_timer_handle_t reboot_timer;
    const esp_timer_create_args_t reboot_args = {
        .callback = reboot_timer_cb,
        .name = "reboot"
    };
    esp_timer_create(&reboot_args, &reboot_timer);
    esp_timer_start_once(reboot_timer, 2000000); /* 2s in microseconds */
    return ESP_OK;
}

static void timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGW(TAG, "AP provisioning timeout (5 min)");
    bool was_portal_only = s_portal_only;
    ap_provisioning_stop();
    if (was_portal_only) {
        wifi_manager_stop_ap();
    }
}

esp_err_t ap_provisioning_start(void)
{
    if (s_active) return ESP_OK;

    /* Get MAC for SSID/password */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32], ap_pass[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "SchoolBell_%02X%02X", mac[4], mac[5]);
    snprintf(ap_pass, sizeof(ap_pass), "SchoolBell_%02X%02X%02X", mac[3], mac[4], mac[5]);

    /* Create AP netif */
    s_ap_netif = esp_netif_create_default_wifi_ap();

    /* Configure SoftAP */
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: SSID='%s' PASS='%s'", ap_ssid, ap_pass);

    /* Start captive DNS */
    captive_dns_start();

    /* Start HTTP server */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 5;
    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = handler_get_root};
    httpd_uri_t uri_204 = {.uri = "/generate_204", .method = HTTP_GET, .handler = handler_redirect};
    httpd_uri_t uri_ios = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handler_redirect};
    httpd_uri_t uri_save = {.uri = "/save", .method = HTTP_POST, .handler = handler_post_save};

    httpd_register_uri_handler(s_httpd, &uri_root);
    httpd_register_uri_handler(s_httpd, &uri_204);
    httpd_register_uri_handler(s_httpd, &uri_ios);
    httpd_register_uri_handler(s_httpd, &uri_save);

    /* Start timeout timer (5 min) */
    s_timeout_timer = xTimerCreate("ap_timeout", pdMS_TO_TICKS(AP_TIMEOUT_MS),
                                   pdFALSE, NULL, timeout_cb);
    xTimerStart(s_timeout_timer, 0);

    s_active = true;
    s_portal_only = false;
    return ESP_OK;
}

esp_err_t ap_provisioning_start_portal_only(void)
{
    if (s_active) return ESP_OK;

    /* Start captive DNS */
    captive_dns_start();

    /* Start HTTP server */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 5;
    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = handler_get_root};
    httpd_uri_t uri_204 = {.uri = "/generate_204", .method = HTTP_GET, .handler = handler_redirect};
    httpd_uri_t uri_ios = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handler_redirect};
    httpd_uri_t uri_save = {.uri = "/save", .method = HTTP_POST, .handler = handler_post_save};

    httpd_register_uri_handler(s_httpd, &uri_root);
    httpd_register_uri_handler(s_httpd, &uri_204);
    httpd_register_uri_handler(s_httpd, &uri_ios);
    httpd_register_uri_handler(s_httpd, &uri_save);

    /* Start timeout timer (5 min) */
    s_timeout_timer = xTimerCreate("ap_timeout", pdMS_TO_TICKS(AP_TIMEOUT_MS),
                                   pdFALSE, NULL, timeout_cb);
    xTimerStart(s_timeout_timer, 0);

    s_active = true;
    s_portal_only = true;
    ESP_LOGI(TAG, "Portal-only started (HTTP + DNS)");
    return ESP_OK;
}

void ap_provisioning_stop(void)
{
    if (!s_active) return;

    if (s_timeout_timer) {
        xTimerStop(s_timeout_timer, 0);
        xTimerDelete(s_timeout_timer, 0);
        s_timeout_timer = NULL;
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    captive_dns_stop();

    if (!s_portal_only) {
        esp_wifi_stop();
        if (s_ap_netif) {
            esp_netif_destroy_default_wifi(s_ap_netif);
            s_ap_netif = NULL;
        }
    }
    /* In portal-only mode, caller is responsible for stopping AP interface */

    s_active = false;
    s_portal_only = false;
    ESP_LOGI(TAG, "AP provisioning stopped");
}

bool ap_provisioning_is_active(void)
{
    return s_active;
}
