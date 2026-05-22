#ifndef RTC_DRIVER_H
#define RTC_DRIVER_H

#include "esp_err.h"
#include <time.h>

/**
 * Initialize I2C and RTC module (DS1307/DS3231).
 * Sets system time from RTC on boot.
 */
esp_err_t rtc_driver_init(void);

/**
 * Read current time from RTC hardware.
 */
esp_err_t rtc_driver_get_time(struct tm *t);

/**
 * Write time to RTC hardware (e.g. after SNTP sync).
 */
esp_err_t rtc_driver_set_time(const struct tm *t);

/**
 * Sync system time from RTC. Call on boot before SNTP is available.
 */
esp_err_t rtc_driver_sync_to_system(void);

/**
 * Save current system time to RTC. Call after SNTP sync.
 */
esp_err_t rtc_driver_save_from_system(void);

#endif
