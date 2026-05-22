#ifndef APP_MQTT_H
#define APP_MQTT_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize and connect MQTT client to broker.
 * Subscribes to device command/schedule/config topics.
 */
esp_err_t mqtt_client_init(const char *device_id, const char *username, const char *password);

/**
 * Publish a message to a topic.
 */
esp_err_t mqtt_client_publish(const char *topic, const char *data, int qos);

/**
 * Check if MQTT is currently connected.
 */
bool mqtt_client_is_connected(void);

/**
 * Publish device heartbeat status.
 */
esp_err_t mqtt_client_send_heartbeat(const char *device_id, int8_t rssi, uint32_t uptime_sec);

#endif
