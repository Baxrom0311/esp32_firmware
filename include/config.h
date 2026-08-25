#ifndef CONFIG_H
#define CONFIG_H
/* WiFi defaults (used if NVS is empty — must be provisioned via AP mode) */
#define DEFAULT_WIFI_SSID       "12"
#define DEFAULT_WIFI_PASS       "12345678"
/* MQTT */
#define MQTT_BROKER_URI         "mqtts://mqtt.boos.uz:8883"
#define MQTT_KEEPALIVE_SEC      30
/* API */
#define API_BASE_URL            "https://bell.boos.uz"
#define API_REGISTER_PATH       "/api/v1/device/auto-register/"
#define API_ACTIVATE_PATH       "/api/v1/device/activate/"
#define API_CREDENTIALS_PATH    "/api/v1/device/credentials/"
#define API_ENTITLEMENT_PATH    "/api/v1/device/entitlement/"
#define ENTITLEMENT_HMAC_SECRET "161b0e7c8a53e74af27c280fe681d087f1f2190e1aae5f4e512a477f0d33ee57"
#define ENTITLEMENT_REQUIRED    1
/* Hardware */
#define BELL_GPIO_PIN           GPIO_NUM_13
#define BELL_DEFAULT_DURATION_MS 3000
#define PANIC_BUTTON_GPIO       GPIO_NUM_0
#define RELAY2_GPIO_PIN         GPIO_NUM_14
#define BELL_MAX_DURATION_MS    30000
/* RTC DS1302 — 3-wire interface (CLK, DAT, RST) */
#define RTC_CLK_PIN             GPIO_NUM_21
#define RTC_DAT_PIN             GPIO_NUM_22
#define RTC_RST_PIN             GPIO_NUM_4
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
#define NVS_NAMESPACE_HOLIDAYS  "holidays"
#define NVS_NAMESPACE_ENTITLEMENT "entitlement"
/* OTA */
#define OTA_ALLOWED_HOST        "bell.boos.uz"
#define OTA_ALLOWED_HOST_2      "67.205.171.93"
/* Firmware version — bump on each release */
#define FW_VERSION              "1.1.0"
/* Hardware revision — bump when PCB, sensors, or GPIO mapping changes */
#define HW_VERSION              "1.0"
/* BLE Provisioning */
#endif /* CONFIG_H */
