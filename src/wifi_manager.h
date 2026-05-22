#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t wifi_manager_init(void);
bool wifi_manager_is_connected(void);
int8_t wifi_manager_get_rssi(void);

#endif
