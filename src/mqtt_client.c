#include "app_mqtt.h"
#include "config.h"
#include "ca_cert.h"
#include "nvs_storage.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "mqtt_client.h" /* ESP-IDF MQTT client */
#include "bell_controller.h"
#include "holiday_manager.h"
#include "ota_manager.h"
#include "schedule_manager.h"
#include "cJSON.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static _Atomic bool s_connected = false;
static char s_device_id[64] = {0};

/* Reassembly buffer for fragmented MQTT messages */
static char *s_frag_buf = NULL;
static int s_frag_len = 0;
static int s_frag_total = 0;
static char s_frag_topic[96] = {0};

/* Topic buffers */
static char s_topic_command[96];
static char s_topic_schedule[96];
static char s_topic_config[96];
static char s_topic_status[96];
static char s_topic_holidays[96];

static void build_topics(const char *device_id)
{
    snprintf(s_topic_command, sizeof(s_topic_command), "devices/%s/command", device_id);
    snprintf(s_topic_schedule, sizeof(s_topic_schedule), "devices/%s/schedule", device_id);
    snprintf(s_topic_config, sizeof(s_topic_config), "devices/%s/config", device_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "devices/%s/status", device_id);
    snprintf(s_topic_holidays, sizeof(s_topic_holidays), "devices/%s/holidays", device_id);
}

static void subscribe_topics(void)
{
    esp_mqtt_client_subscribe(s_client, s_topic_command, 1);
    esp_mqtt_client_subscribe(s_client, s_topic_schedule, 1);
    esp_mqtt_client_subscribe(s_client, s_topic_config, 1);
    esp_mqtt_client_subscribe(s_client, s_topic_holidays, 1);
    ESP_LOGI(TAG, "Subscribed to device topics");
}

static void mqtt_dispatch_command(const char *json_data)
{
    cJSON *root = cJSON_Parse(json_data);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse command JSON");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    if (!cmd || !cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        return;
    }

    const char *command = cmd->valuestring;

    if (strcmp(command, "ring") == 0) {
        cJSON *dur = cJSON_GetObjectItem(root, "duration");
        uint32_t duration_ms = BELL_DEFAULT_DURATION_MS;
        if (dur && cJSON_IsNumber(dur)) {
            int val = dur->valueint;
            if (val > 0 && val <= (int)(BELL_MAX_DURATION_MS / 1000)) {
                duration_ms = (uint32_t)val * 1000;
            }
        }
        bell_controller_ring(duration_ms);
    } else if (strcmp(command, "ota") == 0) {
        cJSON *url = cJSON_GetObjectItem(root, "url");
        if (url && cJSON_IsString(url)) {
            ota_manager_start(url->valuestring, s_device_id);
        }
    } else if (strcmp(command, "reboot") == 0) {
        ESP_LOGI(TAG, "Reboot command received");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (strcmp(command, "silent") == 0) {
        cJSON *state = cJSON_GetObjectItem(root, "state");
        bool on = cJSON_IsTrue(state);
        holiday_manager_set_silent(on);
        ESP_LOGI(TAG, "Silent mode: %s", on ? "ON" : "OFF");
    } else if (strcmp(command, "ntp_sync") == 0) {
        ESP_LOGI(TAG, "NTP sync command received");
        schedule_manager_sync_time();
    } else if (strcmp(command, "fire_alarm") == 0) {
        ESP_LOGW(TAG, "FIRE ALARM! Repeating pattern");
        /* 3s on, 2s off, repeat 6 times = 30s total */
        for (int i = 0; i < 6; i++) {
            bell_controller_ring(3000);
            vTaskDelay(pdMS_TO_TICKS(5000)); /* 3s ring + 2s pause */
        }
    }

    cJSON_Delete(root);
}

static void mqtt_dispatch_config(const char *json_data)
{
    cJSON *root = cJSON_Parse(json_data);
    if (!root) return;

    /* Handle timezone config: {"tz": "UTC-5"} */
    cJSON *tz = cJSON_GetObjectItem(root, "tz");
    if (tz && cJSON_IsString(tz)) {
        nvs_storage_set_str(NVS_NAMESPACE_CONFIG, "tz", tz->valuestring);
        setenv("TZ", tz->valuestring, 1);
        tzset();
        ESP_LOGI(TAG, "Timezone updated to: %s", tz->valuestring);
    }

    cJSON_Delete(root);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_connected = true;
        subscribe_topics();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        /* Free fragment buffer on disconnect */
        free(s_frag_buf);
        s_frag_buf = NULL;
        s_frag_len = 0;
        s_frag_total = 0;
        break;

    case MQTT_EVENT_DATA:
        /* Handle fragmented messages: topic is only present in first fragment */
        if (event->topic_len > 0) {
            /* First (or only) fragment — store topic and allocate buffer */
            free(s_frag_buf);
            s_frag_buf = NULL;
            s_frag_len = 0;
            s_frag_total = event->total_data_len;
            if (event->topic_len < (int)sizeof(s_frag_topic)) {
                memcpy(s_frag_topic, event->topic, event->topic_len);
                s_frag_topic[event->topic_len] = '\0';
            }
            if (s_frag_total > 0 && s_frag_total < 8192) {
                s_frag_buf = malloc(s_frag_total + 1);
            }
        }

        if (s_frag_buf && event->data_len > 0 &&
            (s_frag_len + event->data_len) <= s_frag_total) {
            memcpy(s_frag_buf + s_frag_len, event->data, event->data_len);
            s_frag_len += event->data_len;
        }

        /* Dispatch only when fully reassembled */
        if (s_frag_buf && s_frag_len >= s_frag_total) {
            s_frag_buf[s_frag_len] = '\0';
            ESP_LOGI(TAG, "MQTT data: topic=%s, len=%d", s_frag_topic, s_frag_len);

            if (strcmp(s_frag_topic, s_topic_schedule) == 0) {
                schedule_manager_update(s_frag_buf);
            } else if (strcmp(s_frag_topic, s_topic_command) == 0) {
                mqtt_dispatch_command(s_frag_buf);
            } else if (strcmp(s_frag_topic, s_topic_config) == 0) {
                mqtt_dispatch_config(s_frag_buf);
            } else if (strcmp(s_frag_topic, s_topic_holidays) == 0) {
                holiday_manager_update_full(s_frag_buf);
            }
            free(s_frag_buf);
            s_frag_buf = NULL;
            s_frag_len = 0;
            s_frag_total = 0;
        } else if (!s_frag_buf && event->total_data_len >= 8192) {
            ESP_LOGW(TAG, "MQTT message too large (%d bytes), dropped", event->total_data_len);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type: %d", event->error_handle->error_type);
        break;

    default:
        break;
    }
}

esp_err_t mqtt_client_init(const char *device_id, const char *username, const char *password)
{
    if (s_client != NULL) {
        return ESP_OK; /* Already initialized */
    }

    strncpy(s_device_id, device_id, sizeof(s_device_id) - 1);
    build_topics(device_id);

    char client_id[80];
    snprintf(client_id, sizeof(client_id), "esp32_%s", device_id);

    /* LWT (Last Will and Testament) payload */
    static const char *lwt_payload = "{\"status\":\"offline\"}";

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        /* .broker.verification.certificate = ca_cert_pem, */ /* disabled: using mqtt:// not mqtts:// */
        .credentials = {
            .username = username,
            .client_id = client_id,
            .authentication = {
                .password = password,
            },
        },
        .session.keepalive = MQTT_KEEPALIVE_SEC,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = lwt_payload,
            .msg_len = 0,
            .qos = 1,
            .retain = 1,
        },
        .network.reconnect_timeout_ms = MQTT_RECONNECT_DELAY_MS,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t mqtt_client_publish(const char *topic, const char *data, int qos)
{
    if (!s_connected || s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, data, 0, qos, 0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

bool mqtt_client_is_connected(void)
{
    return s_connected;
}

esp_err_t mqtt_client_send_heartbeat(const char *device_id, int8_t rssi, uint32_t uptime_sec)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "devices/%s/status", device_id);

    uint32_t free_heap = esp_get_free_heap_size();
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"status\":\"online\",\"rssi\":%d,\"uptime\":%lu,\"fw\":\"%s\",\"heap\":%lu}",
             rssi, (unsigned long)uptime_sec, FW_VERSION, (unsigned long)free_heap);

    return mqtt_client_publish(topic, payload, 0);
}
