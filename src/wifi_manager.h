#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef void (*wifi_ap_fallback_cb_t)(void);

/* Initialize WiFi STA with AP fallback. Tries 3 times, then calls cb. */
esp_err_t wifi_manager_init_with_fallback(wifi_ap_fallback_cb_t cb);

/* Legacy init (no fallback) */
esp_err_t wifi_manager_init(void);

/* Start AP+STA mode: AP for provisioning, STA retries in background (30s) */
esp_err_t wifi_manager_start_apsta(void);

/* Stop only the AP interface (keep STA running) */
void wifi_manager_stop_ap(void);

/* Check if AP is currently active (AP+STA mode) */
bool wifi_manager_is_ap_active(void);

/* Stop STA mode (before starting AP) */
void wifi_manager_stop_sta(void);

/* Restart STA mode (after AP provisioning) */
void wifi_manager_restart_sta(void);

bool wifi_manager_is_connected(void);
int8_t wifi_manager_get_rssi(void);

#endif
