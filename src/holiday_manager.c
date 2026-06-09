#include "holiday_manager.h"
#include "app_mqtt.h"
#include "wifi_manager.h"
#include "config.h"

#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_crc.h"
#include "nvs_flash.h"
#include "cJSON.h"

static const char *TAG = "holiday_mgr";

static holiday_entry_t s_holidays[MAX_HOLIDAYS];
static uint8_t s_holiday_count = 0;
static holiday_range_t s_ranges[MAX_HOLIDAY_RANGES];
static uint8_t s_range_count = 0;
static bool s_silent = false;
static uint32_t s_version = 0;

static bool is_date_in_range(uint8_t month, uint8_t day, const holiday_range_t *r) {
    /* Normal range: from <= to (e.g. 6/1 - 8/31) */
    if (r->from_month < r->to_month ||
        (r->from_month == r->to_month && r->from_day <= r->to_day)) {
        if (month > r->from_month || (month == r->from_month && day >= r->from_day)) {
            if (month < r->to_month || (month == r->to_month && day <= r->to_day)) {
                return true;
            }
        }
        return false;
    }
    /* Year-crossing range: from > to (e.g. 12/25 - 1/5) */
    if (month > r->from_month || (month == r->from_month && day >= r->from_day)) return true;
    if (month < r->to_month || (month == r->to_month && day <= r->to_day)) return true;
    return false;
}

static esp_err_t save_to_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_HOLIDAYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    /* Save dates */
    size_t dates_len = s_holiday_count * sizeof(holiday_entry_t);
    nvs_set_blob(h, "data", s_holidays, dates_len);
    nvs_set_u8(h, "count", s_holiday_count);

    /* Save ranges */
    size_t ranges_len = s_range_count * sizeof(holiday_range_t);
    nvs_set_blob(h, "ranges", s_ranges, ranges_len);
    nvs_set_u8(h, "rcnt", s_range_count);

    /* CRC over both */
    uint32_t crc = esp_crc32_le(0, (const uint8_t *)s_holidays, dates_len);
    crc = esp_crc32_le(crc, (const uint8_t *)s_ranges, ranges_len);
    nvs_set_blob(h, "crc", &crc, sizeof(crc));

    nvs_set_u8(h, "silent", s_silent ? 1 : 0);
    nvs_set_u32(h, "ver", s_version);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t holiday_manager_init(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_HOLIDAYS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No holiday data in NVS");
        return ESP_OK;
    }

    nvs_get_u8(h, "count", &s_holiday_count);
    if (s_holiday_count > MAX_HOLIDAYS) s_holiday_count = MAX_HOLIDAYS;

    nvs_get_u8(h, "rcnt", &s_range_count);
    if (s_range_count > MAX_HOLIDAY_RANGES) s_range_count = MAX_HOLIDAY_RANGES;

    size_t dates_len = s_holiday_count * sizeof(holiday_entry_t);
    if (dates_len > 0) nvs_get_blob(h, "data", s_holidays, &dates_len);

    size_t ranges_len = s_range_count * sizeof(holiday_range_t);
    if (ranges_len > 0) nvs_get_blob(h, "ranges", s_ranges, &ranges_len);

    /* CRC validation */
    uint32_t stored_crc = 0;
    size_t crc_len = sizeof(stored_crc);
    esp_err_t crc_err = nvs_get_blob(h, "crc", &stored_crc, &crc_len);
    uint32_t calc_crc = esp_crc32_le(0, (const uint8_t *)s_holidays, dates_len);
    calc_crc = esp_crc32_le(calc_crc, (const uint8_t *)s_ranges, ranges_len);
    if (crc_err != ESP_OK || stored_crc != calc_crc) {
        ESP_LOGE(TAG, "Holiday CRC mismatch, discarding");
        s_holiday_count = 0;
        s_range_count = 0;
        nvs_close(h);
        return ESP_OK;
    }

    uint8_t silent_val = 0;
    nvs_get_u8(h, "silent", &silent_val);
    s_silent = (silent_val != 0);

    nvs_get_u32(h, "ver", &s_version);

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %d dates, %d ranges, ver=%lu, silent=%d",
             s_holiday_count, s_range_count, (unsigned long)s_version, s_silent);
    return ESP_OK;
}

bool holiday_manager_is_today_holiday(void) {
    time_t now;
    time(&now);
    if (now < 1577836800) return false; /* Time not synced yet */

    struct tm t;
    localtime_r(&now, &t);
    uint8_t month = t.tm_mon + 1;
    uint8_t day = t.tm_mday;

    /* Check ranges first */
    for (int i = 0; i < s_range_count; i++) {
        if (is_date_in_range(month, day, &s_ranges[i])) return true;
    }
    /* Check individual dates */
    for (int i = 0; i < s_holiday_count; i++) {
        if (s_holidays[i].month == month && s_holidays[i].day == day) return true;
    }
    return false;
}

bool holiday_manager_is_silent(void) {
    return s_silent;
}

esp_err_t holiday_manager_update_full(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_ERR_INVALID_ARG;

    /* Version check */
    cJSON *ver = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsNumber(ver)) {
        uint32_t new_ver = (uint32_t)ver->valueint;
        if (new_ver <= s_version) {
            ESP_LOGI(TAG, "Skipping older version %lu (current %lu)",
                     (unsigned long)new_ver, (unsigned long)s_version);
            cJSON_Delete(root);
            return ESP_OK;
        }
        s_version = new_ver;
    }

    /* Extract msg_id for ACK */
    cJSON *msg_id = cJSON_GetObjectItem(root, "msg_id");

    /* Parse ranges */
    cJSON *ranges_arr = cJSON_GetObjectItem(root, "ranges");
    s_range_count = 0;
    if (cJSON_IsArray(ranges_arr)) {
        int cnt = cJSON_GetArraySize(ranges_arr);
        if (cnt > MAX_HOLIDAY_RANGES) cnt = MAX_HOLIDAY_RANGES;
        for (int i = 0; i < cnt; i++) {
            cJSON *item = cJSON_GetArrayItem(ranges_arr, i);
            cJSON *fm = cJSON_GetObjectItem(item, "from_month");
            cJSON *fd = cJSON_GetObjectItem(item, "from_day");
            cJSON *tm = cJSON_GetObjectItem(item, "to_month");
            cJSON *td = cJSON_GetObjectItem(item, "to_day");
            if (cJSON_IsNumber(fm) && cJSON_IsNumber(fd) &&
                cJSON_IsNumber(tm) && cJSON_IsNumber(td)) {
                int fmv = fm->valueint, fdv = fd->valueint;
                int tmv = tm->valueint, tdv = td->valueint;
                if (fmv >= 1 && fmv <= 12 && fdv >= 1 && fdv <= 31 &&
                    tmv >= 1 && tmv <= 12 && tdv >= 1 && tdv <= 31) {
                    s_ranges[s_range_count].from_month = (uint8_t)fmv;
                    s_ranges[s_range_count].from_day = (uint8_t)fdv;
                    s_ranges[s_range_count].to_month = (uint8_t)tmv;
                    s_ranges[s_range_count].to_day = (uint8_t)tdv;
                    s_range_count++;
                } else {
                    ESP_LOGW(TAG, "Skipping invalid range: %d/%d - %d/%d", fmv, fdv, tmv, tdv);
                }
            }
        }
    }

    /* Parse dates */
    cJSON *dates_arr = cJSON_GetObjectItem(root, "dates");
    s_holiday_count = 0;
    if (cJSON_IsArray(dates_arr)) {
        int cnt = cJSON_GetArraySize(dates_arr);
        if (cnt > MAX_HOLIDAYS) cnt = MAX_HOLIDAYS;
        for (int i = 0; i < cnt; i++) {
            cJSON *item = cJSON_GetArrayItem(dates_arr, i);
            cJSON *m = cJSON_GetObjectItem(item, "month");
            cJSON *d = cJSON_GetObjectItem(item, "day");
            if (cJSON_IsNumber(m) && cJSON_IsNumber(d)) {
                int mv = m->valueint, dv = d->valueint;
                if (mv >= 1 && mv <= 12 && dv >= 1 && dv <= 31) {
                    s_holidays[s_holiday_count].month = (uint8_t)mv;
                    s_holidays[s_holiday_count].day = (uint8_t)dv;
                    s_holiday_count++;
                } else {
                    ESP_LOGW(TAG, "Skipping invalid date: %d/%d", mv, dv);
                }
            }
        }
    }

    /* Parse silent */
    cJSON *silent = cJSON_GetObjectItem(root, "silent");
    if (cJSON_IsBool(silent)) {
        s_silent = cJSON_IsTrue(silent);
    }

    ESP_LOGI(TAG, "Updated: %d ranges, %d dates, ver=%lu, silent=%d",
             s_range_count, s_holiday_count, (unsigned long)s_version, s_silent);

    esp_err_t err = save_to_nvs();

    /* Send ACK after successful save */
    if (err == ESP_OK && msg_id && cJSON_IsString(msg_id)) {
        char ack_topic[96], ack_payload[128];
        snprintf(ack_topic, sizeof(ack_topic), "devices/%s/ack", mqtt_client_get_device_id());
        snprintf(ack_payload, sizeof(ack_payload),
            "{\"msg_id\":\"%s\",\"status\":\"ok\"}", msg_id->valuestring);
        mqtt_client_publish(ack_topic, ack_payload, 1);
    }

    cJSON_Delete(root);
    return err;
}

esp_err_t holiday_manager_update(const char *json) {
    /* Legacy format: {"holidays": [...]} — redirect to full parser with wrapper */
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_ERR_INVALID_ARG;

    /* Check if it's the new format (has "version" key) */
    if (cJSON_GetObjectItem(root, "version")) {
        cJSON_Delete(root);
        return holiday_manager_update_full(json);
    }

    /* Old format */
    cJSON *arr = cJSON_GetObjectItem(root, "holidays");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int count = cJSON_GetArraySize(arr);
    if (count > MAX_HOLIDAYS) count = MAX_HOLIDAYS;

    s_holiday_count = 0;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *m = cJSON_GetObjectItem(item, "month");
        cJSON *d = cJSON_GetObjectItem(item, "day");
        if (cJSON_IsNumber(m) && cJSON_IsNumber(d)) {
            int mv = m->valueint, dv = d->valueint;
            if (mv >= 1 && mv <= 12 && dv >= 1 && dv <= 31) {
                s_holidays[s_holiday_count].month = (uint8_t)mv;
                s_holidays[s_holiday_count].day = (uint8_t)dv;
                s_holiday_count++;
            }
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Updated %d holidays (legacy format)", s_holiday_count);
    return save_to_nvs();
}

esp_err_t holiday_manager_set_silent(bool silent) {
    s_silent = silent;
    ESP_LOGI(TAG, "Silent mode: %s", silent ? "ON" : "OFF");
    return save_to_nvs();
}
