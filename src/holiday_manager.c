#include "holiday_manager.h"
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
static bool s_silent = false;

static esp_err_t save_to_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_HOLIDAYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    size_t blob_len = s_holiday_count * sizeof(holiday_entry_t);
    nvs_set_blob(h, "data", s_holidays, blob_len);
    uint32_t crc = esp_crc32_le(0, (const uint8_t *)s_holidays, blob_len);
    nvs_set_blob(h, "crc", &crc, sizeof(crc));
    nvs_set_u8(h, "count", s_holiday_count);
    nvs_set_u8(h, "silent", s_silent ? 1 : 0);
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

    size_t len = s_holiday_count * sizeof(holiday_entry_t);
    if (len > 0) {
        nvs_get_blob(h, "data", s_holidays, &len);

        /* CRC validation */
        uint32_t stored_crc = 0;
        size_t crc_len = sizeof(stored_crc);
        esp_err_t crc_err = nvs_get_blob(h, "crc", &stored_crc, &crc_len);
        uint32_t calc_crc = esp_crc32_le(0, (const uint8_t *)s_holidays, len);
        if (crc_err != ESP_OK || stored_crc != calc_crc) {
            ESP_LOGE(TAG, "Holiday CRC mismatch, discarding");
            s_holiday_count = 0;
            nvs_close(h);
            return ESP_OK;
        }
    }

    uint8_t silent_val = 0;
    nvs_get_u8(h, "silent", &silent_val);
    s_silent = (silent_val != 0);

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %d holidays (CRC OK), silent=%d", s_holiday_count, s_silent);
    return ESP_OK;
}

bool holiday_manager_is_today_holiday(void) {
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);

    uint8_t month = t.tm_mon + 1;
    uint8_t day = t.tm_mday;

    for (int i = 0; i < s_holiday_count; i++) {
        if (s_holidays[i].month == month && s_holidays[i].day == day) {
            return true;
        }
    }
    return false;
}

bool holiday_manager_is_silent(void) {
    return s_silent;
}

esp_err_t holiday_manager_update(const char *json) {
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "Holiday update rejected: offline");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_ERR_INVALID_ARG;

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
            s_holidays[s_holiday_count].month = (uint8_t)m->valueint;
            s_holidays[s_holiday_count].day = (uint8_t)d->valueint;
            s_holiday_count++;
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Updated %d holidays", s_holiday_count);
    return save_to_nvs();
}

esp_err_t holiday_manager_set_silent(bool silent) {
    s_silent = silent;
    ESP_LOGI(TAG, "Silent mode: %s", silent ? "ON" : "OFF");
    return save_to_nvs();
}
