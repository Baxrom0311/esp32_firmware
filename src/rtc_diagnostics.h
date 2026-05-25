#ifndef RTC_DIAGNOSTICS_H
#define RTC_DIAGNOSTICS_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t rtc_diagnostics_init(void);
void rtc_diagnostics_tick(void);
bool rtc_diagnostics_battery_dead(void);
int32_t rtc_diagnostics_last_drift_sec(void);

/**
 * Get battery status as string: "ok", "low", or "dead".
 */
const char *rtc_diagnostics_battery_status(void);

/**
 * Get consecutive days with drift > 5 min (0-255).
 */
uint8_t rtc_diagnostics_consecutive_drift_days(void);

#endif
