#include "rtc_driver.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include <sys/time.h>

static const char *TAG = "rtc";

static uint8_t bcd_to_dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t dec_to_bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

static esp_err_t i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = RTC_I2C_SDA_PIN,
        .scl_io_num = RTC_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = RTC_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(RTC_I2C_PORT, &conf);
    if (err != ESP_OK) return err;
    return i2c_driver_install(RTC_I2C_PORT, conf.mode, 0, 0, 0);
}

esp_err_t rtc_driver_init(void)
{
    esp_err_t err = i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Sync RTC time to system on boot */
    err = rtc_driver_sync_to_system();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC read failed, will rely on SNTP");
    }
    return ESP_OK;
}

esp_err_t rtc_driver_get_time(struct tm *t)
{
    uint8_t reg = 0x00;
    uint8_t data[7];

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (RTC_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (RTC_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, 7, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(RTC_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK) return err;

    t->tm_sec  = bcd_to_dec(data[0] & 0x7F);
    t->tm_min  = bcd_to_dec(data[1] & 0x7F);
    t->tm_hour = bcd_to_dec(data[2] & 0x3F);
    t->tm_wday = bcd_to_dec(data[3] & 0x07) - 1;
    t->tm_mday = bcd_to_dec(data[4] & 0x3F);
    t->tm_mon  = bcd_to_dec(data[5] & 0x1F) - 1;
    t->tm_year = bcd_to_dec(data[6]) + 100; /* years since 1900 */

    return ESP_OK;
}

esp_err_t rtc_driver_set_time(const struct tm *t)
{
    uint8_t data[8];
    data[0] = 0x00; /* register address */
    data[1] = dec_to_bcd(t->tm_sec);
    data[2] = dec_to_bcd(t->tm_min);
    data[3] = dec_to_bcd(t->tm_hour);
    data[4] = dec_to_bcd(t->tm_wday + 1);
    data[5] = dec_to_bcd(t->tm_mday);
    data[6] = dec_to_bcd(t->tm_mon + 1);
    data[7] = dec_to_bcd(t->tm_year - 100);

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (RTC_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, data, 8, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(RTC_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);

    return err;
}

esp_err_t rtc_driver_sync_to_system(void)
{
    struct tm t;
    esp_err_t err = rtc_driver_get_time(&t);
    if (err != ESP_OK) return err;

    time_t epoch = mktime(&t);
    if (epoch < 1700000000) { /* sanity: must be after 2023 */
        ESP_LOGW(TAG, "RTC time invalid (epoch=%ld)", (long)epoch);
        return ESP_ERR_INVALID_STATE;
    }

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "System time set from RTC: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    return ESP_OK;
}

esp_err_t rtc_driver_save_from_system(void)
{
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    esp_err_t err = rtc_driver_set_time(&t);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RTC updated from system time");
    }
    return err;
}
