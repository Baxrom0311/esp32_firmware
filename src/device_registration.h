#ifndef DEVICE_REGISTRATION_H
#define DEVICE_REGISTRATION_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * Check if device is already registered (has device_id in NVS).
 */
bool device_registration_is_registered(void);

/**
 * Register device with the backend API.
 * Stores device_id and MQTT credentials in NVS on success.
 */
esp_err_t device_registration_register(void);

/**
 * Register with exponential backoff retry (3 attempts: 5s, 15s, 45s).
 */
esp_err_t device_registration_register_with_retry(void);

/**
 * Get stored device ID. Returns NULL if not registered.
 */
const char *device_registration_get_id(void);

/**
 * Get stored MQTT username.
 */
const char *device_registration_get_mqtt_user(void);

/**
 * Get stored MQTT password.
 */
const char *device_registration_get_mqtt_pass(void);

/**
 * Fetch MQTT credentials from server using API key (POST).
 * Stores credentials in NVS on success.
 */
esp_err_t device_registration_fetch_credentials(const char *api_key);

#endif
