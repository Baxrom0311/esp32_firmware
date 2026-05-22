#include "schedule_manager.h"
#include "bell_controller.h"
#include "nvs_storage.h"
#include "config.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "cJSON.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "schedule";

static schedule_entry_t s_entries[MAX_SCHEDULE_ENTRIES];
static int s_entry_count = 0;

static uint32_t compute_crc(const void *data, size_t len)
{
    return esp_crc32_le(0, (const uint8_t *)data, len);
}

esp_err_t schedule_manager_init(void)
{
    size_t len = sizeof(s_entries);
    esp_err_t err = nvs_storage_get_blob(NVS_NAMESPACE_SCHEDULE, "data", s_entries, &len);
    if (err == ESP_OK && len > 0) {
        /* Validate CRC32 */
        uint32_t stored_crc = 0;
        size_t crc_len = sizeof(stored_crc);
        esp_err_t crc_err = nvs_storage_get_blob(NVS_NAMESPACE_SCHEDULE, "crc", &stored_crc, &crc_len);
        uint32_t calc_crc = compute_crc(s_entries, len);

        if (crc_err != ESP_OK || stored_crc != calc_crc) {
            ESP_LOGE(TAG, "Schedule CRC mismatch (stored=0x%08lx calc=0x%08lx), discarding",
                     (unsigned long)stored_crc, (unsigned long)calc_crc);
            nvs_storage_erase_namespace(NVS_NAMESPACE_SCHEDULE);
            s_entry_count = 0;
            return ESP_OK;
        }

        s_entry_count = len / sizeof(schedule_entry_t);
        if (s_entry_count > MAX_SCHEDULE_ENTRIES) {
            ESP_LOGE(TAG, "NVS entry count %d exceeds max %d, discarding", s_entry_count, MAX_SCHEDULE_ENTRIES);
            nvs_storage_erase_namespace(NVS_NAMESPACE_SCHEDULE);
            s_entry_count = 0;
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Loaded %d schedule entries from NVS (CRC OK)", s_entry_count);
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
    cJSON *root = cJSON_Parse(json_payload);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse schedule JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *entries = cJSON_GetObjectItem(root, "entries");
    if (!cJSON_IsArray(entries)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

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
        s_entries[count].hour = (uint8_t)h;
        s_entries[count].minute = (uint8_t)m;
        cJSON *dur = cJSON_GetObjectItem(item, "duration");
        s_entries[count].duration_ms = (dur && cJSON_IsNumber(dur)) ?
            dur->valueint : BELL_DEFAULT_DURATION_MS;
        cJSON *days = cJSON_GetObjectItem(item, "days");
        s_entries[count].days_mask = (days && cJSON_IsNumber(days)) ?
            days->valueint : 0x7F;
        count++;
    }
    s_entry_count = count;
    cJSON_Delete(root);

    /* Save blob + CRC32 */
    size_t blob_len = count * sizeof(schedule_entry_t);
    esp_err_t err = nvs_storage_set_blob(NVS_NAMESPACE_SCHEDULE, "data", s_entries, blob_len);
    if (err == ESP_OK) {
        uint32_t crc = compute_crc(s_entries, blob_len);
        nvs_storage_set_blob(NVS_NAMESPACE_SCHEDULE, "crc", &crc, sizeof(crc));
    }

    ESP_LOGI(TAG, "Schedule updated: %d entries, NVS save: %s", count, esp_err_to_name(err));
    return err;
}

void schedule_manager_tick(void)
{
    if (s_entry_count == 0) return;

    time_t now;
    struct tm timeinfo;
    time(&now);

    /* Time validity check: skip if SNTP hasn't synced yet (pre-2020 timestamp) */
    if (now < 1577836800) { /* 2020-01-01 00:00:00 UTC */
        return;
    }

    localtime_r(&now, &timeinfo);

    uint8_t cur_hour = timeinfo.tm_hour;
    uint8_t cur_min = timeinfo.tm_min;
    uint8_t cur_wday = (timeinfo.tm_wday + 6) % 7; /* Convert Sun=0 to Mon=0 */

    /* Use combined hour:minute to prevent re-triggering within same minute
     * but allow same minute in different hours (e.g., 08:30 and 09:30) */
    static uint16_t s_last_triggered_hhmm = 0xFFFF;
    uint16_t cur_hhmm = (uint16_t)cur_hour * 60 + cur_min;

    if (cur_hhmm == s_last_triggered_hhmm) return;

    bool triggered = false;
    for (int i = 0; i < s_entry_count; i++) {
        if (s_entries[i].hour == cur_hour && s_entries[i].minute == cur_min) {
            if (s_entries[i].days_mask & (1 << cur_wday)) {
                ESP_LOGI(TAG, "Triggering bell: entry %d (%02d:%02d)",
                         i, cur_hour, cur_min);
                bell_controller_ring(s_entries[i].duration_ms);
                triggered = true;
            }
        }
    }

    if (triggered) {
        s_last_triggered_hhmm = cur_hhmm;
    }
}

void schedule_manager_sync_time(void)
{
    ESP_LOGI(TAG, "Forcing NTP time sync");
    esp_sntp_restart();
}
