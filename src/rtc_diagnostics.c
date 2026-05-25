#include "rtc_diagnostics.h"
#include "rtc_driver.h"
#include "nvs_storage.h"
#include "app_mqtt.h"
#include "device_registration.h"
#include "config.h"

#include "esp_log.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "rtc_diag";

#define RTC_DRIFT_CORRECT_SEC  30
#define RTC_DRIFT_ALERT_SEC    300
#define RTC_DEAD_CONSECUTIVE   3
#define RTC_CHECK_HOUR         3

static bool s_battery_dead = false;
static int32_t s_last_drift = 0;
static uint8_t s_consecutive_drift_days = 0;
static uint8_t s_last_check_day = 0xFF;

esp_err_t rtc_diagnostics_init(void)
{
    /* Load persisted state */
    uint8_t dead = 0;
    size_t len = sizeof(dead);
    if (nvs_storage_get_blob(NVS_NAMESPACE_CONFIG, "rtc_dead", &dead, &len) == ESP_OK) {
        s_battery_dead = (dead != 0);
    }
    len = sizeof(s_consecutive_drift_days);
    nvs_storage_get_blob(NVS_NAMESPACE_CONFIG, "rtc_ddays", &s_consecutive_drift_days, &len);

    /* Boot check: if RTC year < 2024, battery is likely dead */
    struct tm t;
    if (rtc_driver_get_time(&t) == ESP_OK && (t.tm_year + 1900) < 2024) {
        s_battery_dead = true;
        ESP_LOGW(TAG, "RTC time invalid (year=%d), battery likely dead", t.tm_year + 1900);
    }

    ESP_LOGI(TAG, "Init: battery_dead=%d, consecutive_days=%d", s_battery_dead, s_consecutive_drift_days);
    return ESP_OK;
}

void rtc_diagnostics_tick(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) return; /* SNTP not synced yet */

    struct tm tm_now;
    localtime_r(&now, &tm_now);

    /* Only run at RTC_CHECK_HOUR:00, once per day */
    if (tm_now.tm_hour != RTC_CHECK_HOUR) return;
    if (tm_now.tm_mday == s_last_check_day) return;
    s_last_check_day = tm_now.tm_mday;

    /* Compare SNTP (system) time vs RTC hardware time */
    struct tm rtc_tm;
    if (rtc_driver_get_time(&rtc_tm) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read RTC");
        return;
    }

    time_t rtc_epoch = mktime(&rtc_tm);
    int32_t drift = (int32_t)(now - rtc_epoch);
    s_last_drift = drift;
    int32_t abs_drift = drift < 0 ? -drift : drift;

    ESP_LOGI(TAG, "RTC drift: %ld sec", (long)drift);

    /* Auto-correct if drift > 30s */
    if (abs_drift > RTC_DRIFT_CORRECT_SEC) {
        rtc_driver_save_from_system();
        ESP_LOGI(TAG, "RTC auto-corrected (drift=%lds)", (long)drift);
    }

    /* Alert if drift > 5 min */
    if (abs_drift > RTC_DRIFT_ALERT_SEC) {
        s_consecutive_drift_days++;
        /* Send MQTT alert */
        if (mqtt_client_is_connected()) {
            char topic[96], payload[128];
            const char *dev_id = device_registration_get_id();
            if (dev_id && dev_id[0]) {
                snprintf(topic, sizeof(topic), "devices/%s/alert", dev_id);
                snprintf(payload, sizeof(payload),
                         "{\"type\":\"rtc_drift\",\"drift_sec\":%ld,\"battery_status\":\"%s\"}",
                         (long)drift, s_consecutive_drift_days >= RTC_DEAD_CONSECUTIVE ? "dead" : "low");
                mqtt_client_publish(topic, payload, 1);
            }
        }
    } else {
        /* Drift OK — reset counter and clear dead flag if was set */
        if (s_consecutive_drift_days > 0 || s_battery_dead) {
            s_consecutive_drift_days = 0;
            s_battery_dead = false;
            uint8_t zero = 0;
            nvs_storage_set_blob(NVS_NAMESPACE_CONFIG, "rtc_dead", &zero, sizeof(zero));
            nvs_storage_set_blob(NVS_NAMESPACE_CONFIG, "rtc_ddays", &s_consecutive_drift_days, sizeof(s_consecutive_drift_days));
            ESP_LOGI(TAG, "RTC drift OK, battery status cleared");
        }
        return;
    }

    /* 3 consecutive days → battery dead */
    if (s_consecutive_drift_days >= RTC_DEAD_CONSECUTIVE) {
        s_battery_dead = true;
        ESP_LOGW(TAG, "RTC battery dead (3 consecutive drift days)");
    }

    /* Persist state */
    uint8_t dead_val = s_battery_dead ? 1 : 0;
    nvs_storage_set_blob(NVS_NAMESPACE_CONFIG, "rtc_dead", &dead_val, sizeof(dead_val));
    nvs_storage_set_blob(NVS_NAMESPACE_CONFIG, "rtc_ddays", &s_consecutive_drift_days, sizeof(s_consecutive_drift_days));
}

bool rtc_diagnostics_battery_dead(void)
{
    return s_battery_dead;
}

int32_t rtc_diagnostics_last_drift_sec(void)
{
    return s_last_drift;
}

const char *rtc_diagnostics_battery_status(void)
{
    if (s_battery_dead) return "dead";
    if (s_consecutive_drift_days > 0) return "low";
    return "ok";
}

uint8_t rtc_diagnostics_consecutive_drift_days(void)
{
    return s_consecutive_drift_days;
}
