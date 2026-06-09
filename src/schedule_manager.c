#include "schedule_manager.h"
#include "app_mqtt.h"
#include "bell_controller.h"
#include "holiday_manager.h"
#include "wifi_manager.h"
#include "nvs_storage.h"
#include "config.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "schedule";

#define SCHEDULE_STALE_SEC (7 * 24 * 3600)  /* 7 days */

static schedule_entry_t s_entries[MAX_SCHEDULE_ENTRIES];
static int s_entry_count = 0;
static uint32_t s_schedule_version = 0;
static time_t s_last_sync_time = 0;  /* epoch when schedule was last updated */
static SemaphoreHandle_t s_schedule_mutex = NULL;

static uint32_t compute_crc(const void *data, size_t len)
{
    return esp_crc32_le(0, (const uint8_t *)data, len);
}

esp_err_t schedule_manager_init(void)
{
    if (!s_schedule_mutex) {
        s_schedule_mutex = xSemaphoreCreateMutex();
    }

    size_t len = sizeof(s_entries);
    esp_err_t err = nvs_storage_get_blob(NVS_NAMESPACE_SCHEDULE, "data", s_entries, &len);
    if (err == ESP_OK && len > 0) {
        /* Validate CRC32 */
        uint32_t stored_crc = 0;
        size_t crc_len = sizeof(stored_crc);
        esp_err_t crc_err = nvs_storage_get_blob(NVS_NAMESPACE_SCHEDULE, "crc", &stored_crc, &crc_len);
        uint32_t calc_crc = compute_crc(s_entries, len);

        if (crc_err != ESP_OK || stored_crc != calc_crc) {
            ESP_LOGW(TAG, "Schedule CRC mismatch (stored=0x%08lx calc=0x%08lx), keeping data as fallback",
                     (unsigned long)stored_crc, (unsigned long)calc_crc);
            /* Keep potentially corrupted data as fallback rather than erasing.
             * Device will still ring (possibly wrong times) until next MQTT sync,
             * which is better than no bells at all when offline. */
            s_entry_count = len / sizeof(schedule_entry_t);
            if (s_entry_count > MAX_SCHEDULE_ENTRIES) s_entry_count = MAX_SCHEDULE_ENTRIES;
            return ESP_OK;
        }

        s_entry_count = len / sizeof(schedule_entry_t);
        if (s_entry_count > MAX_SCHEDULE_ENTRIES) {
            ESP_LOGE(TAG, "NVS entry count %d exceeds max %d, discarding", s_entry_count, MAX_SCHEDULE_ENTRIES);
            nvs_storage_erase_namespace(NVS_NAMESPACE_SCHEDULE);
            s_entry_count = 0;
            return ESP_OK;
        }
        /* Load schedule version */
        size_t ver_len = sizeof(s_schedule_version);
        nvs_storage_get_blob(NVS_NAMESPACE_SCHEDULE, "ver", &s_schedule_version, &ver_len);
        /* Load last sync timestamp */
        size_t ts_len = sizeof(s_last_sync_time);
        nvs_storage_get_blob(NVS_NAMESPACE_SCHEDULE, "sync_ts", &s_last_sync_time, &ts_len);
        ESP_LOGI(TAG, "Loaded %d schedule entries from NVS (CRC OK), version=%lu",
                 s_entry_count, (unsigned long)s_schedule_version);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_entry_count = 0;
        ESP_LOGW(TAG, "No schedule in NVS, waiting for MQTT sync");
    } else {
        ESP_LOGE(TAG, "NVS schedule read error (%s), erasing corrupt namespace",
                 esp_err_to_name(err));
        nvs_storage_erase_namespace(NVS_NAMESPACE_SCHEDULE);
        s_entry_count = 0;
    }
    return ESP_OK;
}

esp_err_t schedule_manager_update(const char *json_payload)
{
    /* Online-only guard: reject updates when WiFi is not connected */
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "Schedule update rejected: offline");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_Parse(json_payload);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse schedule JSON");
        return ESP_ERR_INVALID_ARG;
    }

    /* Version check: skip if not newer */
    cJSON *ver = cJSON_GetObjectItem(root, "version");
    if (ver && cJSON_IsNumber(ver)) {
        uint32_t new_ver = (uint32_t)ver->valueint;
        if (new_ver <= s_schedule_version) {
            ESP_LOGI(TAG, "Schedule already up-to-date (v%lu >= v%lu)",
                     (unsigned long)s_schedule_version, (unsigned long)new_ver);
            cJSON_Delete(root);
            return ESP_OK;
        }
    }

    /* Extract msg_id before parsing entries */
    cJSON *msg_id = cJSON_GetObjectItem(root, "msg_id");

    cJSON *entries = cJSON_GetObjectItem(root, "entries");
    if (!cJSON_IsArray(entries)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse entries into a temp buffer, then swap under mutex */
    schedule_entry_t tmp_entries[MAX_SCHEDULE_ENTRIES];
    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, entries) {
        if (count >= MAX_SCHEDULE_ENTRIES) break;
        cJSON *hour_item = cJSON_GetObjectItem(item, "hour");
        cJSON *min_item = cJSON_GetObjectItem(item, "minute");
        if (!hour_item || !min_item) continue;
        int h = hour_item->valueint;
        int m = min_item->valueint;
        if (h < 0 || h > 23 || m < 0 || m > 59) {
            ESP_LOGW(TAG, "Skipping invalid time %d:%d", h, m);
            continue;
        }
        tmp_entries[count].hour = (uint8_t)h;
        tmp_entries[count].minute = (uint8_t)m;
        cJSON *dur = cJSON_GetObjectItem(item, "duration");
        int dur_val = (dur && cJSON_IsNumber(dur)) ? dur->valueint : BELL_DEFAULT_DURATION_MS;
        if (dur_val < 100) dur_val = 100;
        if (dur_val > (int)BELL_MAX_DURATION_MS) dur_val = BELL_MAX_DURATION_MS;
        tmp_entries[count].duration_ms = (uint32_t)dur_val;
        cJSON *days = cJSON_GetObjectItem(item, "days");
        tmp_entries[count].days_mask = (days && cJSON_IsNumber(days)) ?
            days->valueint : 0x7F;
        count++;
    }

    /* Atomically swap entries under mutex */
    if (s_schedule_mutex) xSemaphoreTake(s_schedule_mutex, portMAX_DELAY);
    memcpy(s_entries, tmp_entries, count * sizeof(schedule_entry_t));
    s_entry_count = count;
    if (s_schedule_mutex) xSemaphoreGive(s_schedule_mutex);
    if (ver && cJSON_IsNumber(ver)) {
        s_schedule_version = (uint32_t)ver->valueint;
    } else {
        s_schedule_version++;
    }

    /* Save blob + CRC32 + sync timestamp */
    size_t blob_len = count * sizeof(schedule_entry_t);
    esp_err_t err = nvs_storage_set_blob(NVS_NAMESPACE_SCHEDULE, "data", s_entries, blob_len);
    if (err == ESP_OK) {
        uint32_t crc = compute_crc(s_entries, blob_len);
        nvs_storage_set_blob(NVS_NAMESPACE_SCHEDULE, "crc", &crc, sizeof(crc));
        nvs_storage_set_blob(NVS_NAMESPACE_SCHEDULE, "ver", &s_schedule_version, sizeof(s_schedule_version));
        s_last_sync_time = time(NULL);
        nvs_storage_set_blob(NVS_NAMESPACE_SCHEDULE, "sync_ts", &s_last_sync_time, sizeof(s_last_sync_time));

        /* Send ACK with version */
        if (msg_id && cJSON_IsString(msg_id)) {
            char ack_topic[96], ack_payload[160];
            snprintf(ack_topic, sizeof(ack_topic), "devices/%s/ack", mqtt_client_get_device_id());
            snprintf(ack_payload, sizeof(ack_payload),
                "{\"msg_id\":\"%s\",\"status\":\"ok\",\"version\":%lu}",
                msg_id->valuestring, (unsigned long)s_schedule_version);
            mqtt_client_publish(ack_topic, ack_payload, 1);
        }
    }

    ESP_LOGI(TAG, "Schedule updated: %d entries, NVS save: %s", count, esp_err_to_name(err));
    cJSON_Delete(root);
    return err;
}

void schedule_manager_tick(void)
{
    if (s_entry_count == 0) return;

    time_t now;
    struct tm timeinfo;
    time(&now);

    /* Time validity check: skip if time is clearly invalid (pre-2020).
     * RTC (DS1307) provides valid time even without SNTP/WiFi,
     * so this only triggers on first boot with dead RTC battery. */
    if (now < 1577836800) { /* 2020-01-01 00:00:00 UTC */
        static bool s_time_warn_logged = false;
        if (!s_time_warn_logged) {
            ESP_LOGW(TAG, "System time invalid (pre-2020), bells paused until RTC/SNTP provides valid time");
            s_time_warn_logged = true;
        }
        return;
    }

    localtime_r(&now, &timeinfo);

    /* Skip all bells if today is a holiday or silent mode is active */
    if (holiday_manager_is_silent() || holiday_manager_is_today_holiday()) {
        static uint8_t s_skip_logged_day = 0xFF;
        if (s_skip_logged_day != timeinfo.tm_mday) {
            ESP_LOGI(TAG, "Today is holiday, skipping bells");
            s_skip_logged_day = timeinfo.tm_mday;
        }
        return;
    }

    uint8_t cur_hour = timeinfo.tm_hour;
    uint8_t cur_min = timeinfo.tm_min;
    uint8_t cur_wday = (timeinfo.tm_wday + 6) % 7; /* Convert Sun=0 to Mon=0 */

    /* Debug: print time every 30s */
    static time_t last_debug = 0;
    if (now - last_debug >= 30) {
        ESP_LOGI(TAG, "tick: %02d:%02d (wday=%d, entries=%d)", cur_hour, cur_min, cur_wday, s_entry_count);
        last_debug = now;
    }

    /* Use combined day+hour:minute to prevent re-triggering within same minute
     * but allow same time on different days */
    static uint16_t s_last_triggered_hhmm = 0xFFFF;
    static uint8_t s_last_triggered_day = 0xFF;
    uint16_t cur_hhmm = (uint16_t)cur_hour * 60 + cur_min;

    if (cur_hhmm == s_last_triggered_hhmm && timeinfo.tm_mday == s_last_triggered_day) return;

    /* Take a snapshot of entries under mutex to avoid race with MQTT update */
    schedule_entry_t local_entries[MAX_SCHEDULE_ENTRIES];
    int local_count;
    if (s_schedule_mutex) xSemaphoreTake(s_schedule_mutex, portMAX_DELAY);
    local_count = s_entry_count;
    memcpy(local_entries, s_entries, local_count * sizeof(schedule_entry_t));
    if (s_schedule_mutex) xSemaphoreGive(s_schedule_mutex);

    bool triggered = false;
    for (int i = 0; i < local_count; i++) {
        if (local_entries[i].hour == cur_hour && local_entries[i].minute == cur_min) {
            if (local_entries[i].days_mask & (1 << cur_wday)) {
                ESP_LOGI(TAG, "Triggering bell: entry %d (%02d:%02d)",
                         i, cur_hour, cur_min);
                bell_controller_ring(local_entries[i].duration_ms);
                triggered = true;
            }
        }
    }

    if (triggered) {
        s_last_triggered_hhmm = cur_hhmm;
        s_last_triggered_day = timeinfo.tm_mday;
    }
}

void schedule_manager_sync_time(void)
{
    ESP_LOGI(TAG, "Forcing NTP time sync");
    esp_sntp_restart();
}

uint32_t schedule_manager_get_version(void)
{
    return s_schedule_version;
}

bool schedule_manager_is_stale(void)
{
    if (s_last_sync_time == 0 || s_entry_count == 0) return false;
    time_t now = time(NULL);
    if (now < 1700000000) return false; /* SNTP not synced */
    return (now - s_last_sync_time) > SCHEDULE_STALE_SEC;
}

bool schedule_manager_should_sync(uint32_t server_version)
{
    return server_version > s_schedule_version;
}
