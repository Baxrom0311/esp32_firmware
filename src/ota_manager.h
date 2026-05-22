#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include "esp_err.h"

/**
 * Start OTA update from the given firmware URL.
 * Reports progress via MQTT to devices/{device_id}/ota/status.
 */
esp_err_t ota_manager_start(const char *firmware_url, const char *device_id);

#endif
