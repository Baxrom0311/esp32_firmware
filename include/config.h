#ifndef CONFIG_H
#define CONFIG_H
/* WiFi defaults (used if NVS is empty) */
#define DEFAULT_WIFI_SSID       "12"
#define DEFAULT_WIFI_PASS       "12345678"
/* MQTT */
#define MQTT_BROKER_URI         "mqtt://146.190.210.125:1883"
#define MQTT_KEEPALIVE_SEC      30
/* API */
#define API_BASE_URL            "http://bell.boos.uz"
#define API_REGISTER_PATH       "/api/v1/device/auto-register/"
#define API_ACTIVATE_PATH       "/api/v1/device/activate/"
#define API_CREDENTIALS_PATH    "/api/v1/device/credentials/"
/* Hardware */
#define BELL_GPIO_PIN           GPIO_NUM_13
#define BELL_DEFAULT_DURATION_MS 3000
#define PANIC_BUTTON_GPIO       GPIO_NUM_0
#define RELAY2_GPIO_PIN         GPIO_NUM_14
#define BELL_MAX_DURATION_MS    60000
/* RTC (DS1307/DS3231) I2C pins */
#define RTC_I2C_SDA_PIN         GPIO_NUM_22
#define RTC_I2C_SCL_PIN         GPIO_NUM_21
#define RTC_RST_PIN             GPIO_NUM_4
#define RTC_I2C_PORT            I2C_NUM_0
#define RTC_I2C_FREQ_HZ         100000
#define RTC_I2C_ADDR            0x68
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
/* OTA */
#define OTA_ALLOWED_HOST        "bell.boos.uz"
#define OTA_ALLOWED_HOST_2      "146.190.210.125"
/* Firmware version */
#define FW_VERSION              "1.0.0"
/* BLE Provisioning */
#endif /* CONFIG_H */
