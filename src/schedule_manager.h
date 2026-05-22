#ifndef SCHEDULE_MANAGER_H
#define SCHEDULE_MANAGER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_SCHEDULE_ENTRIES 50

typedef struct __attribute__((packed)) {
    uint8_t hour;
    uint8_t minute;
    uint32_t duration_ms;
    uint8_t days_mask; /* bit0=Mon ... bit6=Sun */
} schedule_entry_t;

/**
 * Initialize schedule manager. Loads schedule from NVS.
 */
esp_err_t schedule_manager_init(void);

/**
 * Update schedule from JSON payload (received via MQTT).
 * Saves to NVS for offline operation.
 */
esp_err_t schedule_manager_update(const char *json_payload);

/**
 * Check current time against schedule and ring bell if needed.
 * Call this every ~30 seconds from main loop.
 */
void schedule_manager_tick(void);

/**
 * Force NTP time synchronization.
 */
void schedule_manager_sync_time(void);

#endif
