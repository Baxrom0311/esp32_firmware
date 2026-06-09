#ifndef BELL_CONTROLLER_H
#define BELL_CONTROLLER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize bell GPIO pin and relay 2.
 */
esp_err_t bell_controller_init(void);

/**
 * Ring the bell for the specified duration in milliseconds.
 * Non-blocking: spawns a FreeRTOS task.
 * Publishes bell_log MQTT message after ring completes.
 */
esp_err_t bell_controller_ring(uint32_t duration_ms);

/**
 * Ring with explicit trigger source for bell_log.
 * source: "schedule", "manual", "emergency", "mqtt"
 */
esp_err_t bell_controller_ring_with_source(uint32_t duration_ms, const char *source);

/**
 * Stop an ongoing bell ring immediately.
 */
void bell_controller_stop(void);

/**
 * Force bell GPIO low. Safe to call from panic handler (no RTOS calls).
 */
void bell_controller_panic_off(void);

/**
 * Load offline bell logs from NVS (call at boot).
 */
void bell_controller_load_offline_log(void);

/**
 * Flush queued offline bell logs via MQTT (call after MQTT connects).
 */
void bell_controller_flush_offline_log(void);

#endif
