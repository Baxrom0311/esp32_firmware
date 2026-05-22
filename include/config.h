#ifndef CONFIG_H
#define CONFIG_H

/* WiFi defaults (used if NVS is empty) */
#define DEFAULT_WIFI_SSID       "SchoolDevice"
#define DEFAULT_WIFI_PASS       "changeme123"

/* MQTT */
#define MQTT_BROKER_URI         "mqtts://mqtt.example.com:8883"
#define MQTT_KEEPALIVE_SEC      30

/* API */
#define API_BASE_URL            "https://api.example.com"
#define API_REGISTER_PATH       "/api/v1/device/auto-register/"
#define API_ACTIVATE_PATH       "/api/v1/device/activate/"
#define API_CREDENTIALS_PATH    "/api/v1/device/credentials/"

/* Hardware */
#define BELL_GPIO_PIN           GPIO_NUM_25
#define BELL_DEFAULT_DURATION_MS 3000
#define BELL_MAX_DURATION_MS    30000

/* Timings */
#define HEARTBEAT_INTERVAL_MS   30000
#define WIFI_RECONNECT_DELAY_MS 5000
#define MQTT_RECONNECT_DELAY_MS 5000
#define SNTP_RESYNC_INTERVAL_MS (3600 * 1000)  /* 1 hour */
#define SNTP_RETRY_INTERVAL_MS  (60 * 1000)    /* 1 minute on failure */

/* WiFi exponential backoff */
#define WIFI_BACKOFF_INITIAL_MS 5000
#define WIFI_BACKOFF_MAX_MS     60000

/* Default timezone (Asia/Tashkent = UTC+5) */
#define DEFAULT_TZ_STRING       "UTC-5"

/* NVS namespaces */
#define NVS_NAMESPACE_WIFI      "wifi"
#define NVS_NAMESPACE_MQTT      "mqtt_creds"
#define NVS_NAMESPACE_DEVICE    "device"
#define NVS_NAMESPACE_SCHEDULE  "schedule"
#define NVS_NAMESPACE_CONFIG    "config"

/* OTA */
#define OTA_ALLOWED_HOST        "api.example.com"
#define OTA_ALLOWED_HOST_2      "firmware.example.com"

/* Firmware version */
#define FW_VERSION              "1.0.0"

#endif /* CONFIG_H */
