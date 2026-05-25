#ifndef HOLIDAY_MANAGER_H
#define HOLIDAY_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

#define MAX_HOLIDAYS 30

typedef struct {
    uint8_t month;
    uint8_t day;
} holiday_entry_t;

/**
 * Initialize holiday manager (load from NVS).
 */
esp_err_t holiday_manager_init(void);

/**
 * Check if today is a holiday.
 */
bool holiday_manager_is_today_holiday(void);

/**
 * Check if silent mode is active.
 */
bool holiday_manager_is_silent(void);

/**
 * Update holiday list from JSON payload received via MQTT.
 * JSON format: {"holidays": [{"month":1,"day":1}, ...]}
 */
esp_err_t holiday_manager_update(const char *json);

/**
 * Set or clear silent mode (force mute for today).
 */
esp_err_t holiday_manager_set_silent(bool silent);

#endif // HOLIDAY_MANAGER_H
