#ifndef BELL_CONTROLLER_H
#define BELL_CONTROLLER_H

#include "esp_err.h"

/**
 * Initialize bell GPIO pin.
 */
esp_err_t bell_controller_init(void);

/**
 * Ring the bell for the specified duration in milliseconds.
 * Non-blocking: spawns a FreeRTOS task.
 */
esp_err_t bell_controller_ring(uint32_t duration_ms);

/**
 * Force bell GPIO low. Safe to call from panic handler (no RTOS calls).
 */
void bell_controller_panic_off(void);

#endif
