#ifndef STATUS_REPORTER_H
#define STATUS_REPORTER_H

#include "esp_err.h"

/**
 * Send periodic heartbeat. Call from main loop task.
 * Internally tracks timing — safe to call frequently.
 */
void status_reporter_tick(const char *device_id);

#endif
