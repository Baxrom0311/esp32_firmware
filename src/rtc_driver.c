#include "rtc_driver.h"
#include "config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include <sys/time.h>
#include <string.h>

static const char *TAG = "rtc";

/* DS1302 uses 3-wire: CLK (SCLK), DAT (IO), RST (CE) */
#define CLK_PIN  RTC_I2C_SCL_PIN  /* GPIO 21 */
#define DAT_PIN  RTC_I2C_SDA_PIN  /* GPIO 22 */
#define RST_PIN  RTC_RST_PIN      /* GPIO 4 */

#define DS1302_SEC_REG   0x80
#define DS1302_MIN_REG   0x82
#define DS1302_HOUR_REG  0x84
#define DS1302_DAY_REG   0x86
#define DS1302_MON_REG   0x88
#define DS1302_WDAY_REG  0x8A
#define DS1302_YEAR_REG  0x8C
#define DS1302_WP_REG    0x8E

static uint8_t bcd_to_dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t dec_to_bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

static void ds1302_start(void)
{
    gpio_set_level(RST_PIN, 0);
    ets_delay_us(2);
    gpio_set_level(CLK_PIN, 0);
    ets_delay_us(2);
    gpio_set_level(RST_PIN, 1);
    ets_delay_us(2);
}

static void ds1302_stop(void)
{
    gpio_set_level(RST_PIN, 0);
    ets_delay_us(2);
}

static void ds1302_write_byte(uint8_t byte)
{
    gpio_set_direction(DAT_PIN, GPIO_MODE_OUTPUT);
    for (int i = 0; i < 8; i++) {
        gpio_set_level(DAT_PIN, (byte >> i) & 1);
        ets_delay_us(2);
        gpio_set_level(CLK_PIN, 1);
        ets_delay_us(2);
        gpio_set_level(CLK_PIN, 0);
        ets_delay_us(2);
    }
}

static uint8_t ds1302_read_byte(void)
{
    uint8_t byte = 0;
    gpio_set_direction(DAT_PIN, GPIO_MODE_INPUT);
    for (int i = 0; i < 8; i++) {
        if (gpio_get_level(DAT_PIN)) {
            byte |= (1 << i);
        }
        ets_delay_us(2);
        gpio_set_level(CLK_PIN, 1);
        ets_delay_us(2);
        gpio_set_level(CLK_PIN, 0);
        ets_delay_us(2);
    }
    return byte;
}

static void ds1302_write_reg(uint8_t reg, uint8_t val)
{
    ds1302_start();
    ds1302_write_byte(reg);
    ds1302_write_byte(val);
    ds1302_stop();
}

static uint8_t ds1302_read_reg(uint8_t reg)
{
    ds1302_start();
    ds1302_write_byte(reg | 0x01); /* Read bit */
    uint8_t val = ds1302_read_byte();
    ds1302_stop();
    return val;
}

esp_err_t rtc_driver_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CLK_PIN) | (1ULL << RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io);

    gpio_config_t dat = {
        .pin_bit_mask = (1ULL << DAT_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&dat);

    gpio_set_level(RST_PIN, 0);
    gpio_set_level(CLK_PIN, 0);

    /* Disable write protect */
    ds1302_write_reg(DS1302_WP_REG, 0x00);

    /* Check if clock is halted (bit 7 of seconds register) */
    uint8_t sec = ds1302_read_reg(DS1302_SEC_REG);
    if (sec & 0x80) {
        ESP_LOGW(TAG, "DS1302 clock halted, starting...");
        ds1302_write_reg(DS1302_SEC_REG, 0x00);
    }

    /* Sync to system */
    esp_err_t err = rtc_driver_sync_to_system();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC time invalid, will rely on SNTP");
    }
    return ESP_OK;
}

esp_err_t rtc_driver_get_time(struct tm *t)
{
    uint8_t sec  = ds1302_read_reg(DS1302_SEC_REG) & 0x7F;
    uint8_t min  = ds1302_read_reg(DS1302_MIN_REG) & 0x7F;
    uint8_t hour = ds1302_read_reg(DS1302_HOUR_REG) & 0x3F;
    uint8_t day  = ds1302_read_reg(DS1302_DAY_REG) & 0x3F;
    uint8_t mon  = ds1302_read_reg(DS1302_MON_REG) & 0x1F;
    uint8_t year = ds1302_read_reg(DS1302_YEAR_REG);

    t->tm_sec  = bcd_to_dec(sec);
    t->tm_min  = bcd_to_dec(min);
    t->tm_hour = bcd_to_dec(hour);
    t->tm_mday = bcd_to_dec(day);
    t->tm_mon  = bcd_to_dec(mon) - 1;
    t->tm_year = bcd_to_dec(year) + 100;

    return ESP_OK;
}

esp_err_t rtc_driver_set_time(const struct tm *t)
{
    ds1302_write_reg(DS1302_WP_REG, 0x00);
    ds1302_write_reg(DS1302_SEC_REG, dec_to_bcd(t->tm_sec));
    ds1302_write_reg(DS1302_MIN_REG, dec_to_bcd(t->tm_min));
    ds1302_write_reg(DS1302_HOUR_REG, dec_to_bcd(t->tm_hour));
    ds1302_write_reg(DS1302_DAY_REG, dec_to_bcd(t->tm_mday));
    ds1302_write_reg(DS1302_MON_REG, dec_to_bcd(t->tm_mon + 1));
    ds1302_write_reg(DS1302_YEAR_REG, dec_to_bcd(t->tm_year - 100));
    ds1302_write_reg(DS1302_WP_REG, 0x80);
    return ESP_OK;
}

esp_err_t rtc_driver_sync_to_system(void)
{
    struct tm t;
    rtc_driver_get_time(&t);
    t.tm_isdst = -1;

    /* RTC stores LOCAL time — mktime converts local to UTC epoch */
    time_t epoch = mktime(&t);

    if (epoch < 1700000000) {
        ESP_LOGW(TAG, "RTC time invalid (year=%d)", t.tm_year + 1900);
        return ESP_ERR_INVALID_STATE;
    }

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "System time from DS1302 (local): %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    return ESP_OK;
}

esp_err_t rtc_driver_save_from_system(void)
{
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t); /* Save LOCAL time to RTC */
    esp_err_t err = rtc_driver_set_time(&t);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "DS1302 updated (local): %02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    }
    return err;
}
