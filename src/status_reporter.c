#include "status_reporter.h"
#include "app_mqtt.h"
#include "wifi_manager.h"
#include "device_registration.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include <stdio.h>

static const char *TAG = "status";
static int64_t s_last_report_us = 0;
static int64_t s_boot_time_us = 0;
static bool s_initialized = false;

/* Heap monitoring: track samples to detect leaks */
#define HEAP_HISTORY_SIZE 10
static uint32_t s_heap_history[HEAP_HISTORY_SIZE];
static int s_heap_idx = 0;
static bool s_heap_full = false;
static bool s_heap_alert_sent = false;

static void heap_monitor_update(uint32_t free_heap)
{
    s_heap_history[s_heap_idx] = free_heap;
    s_heap_idx = (s_heap_idx + 1) % HEAP_HISTORY_SIZE;
    if (s_heap_idx == 0) s_heap_full = true;

    if (!s_heap_full) return;

    /* Check if heap is monotonically declining (oldest → newest).
     * s_heap_idx points to the oldest sample (circular buffer). */
    bool declining = true;
    for (int i = 0; i < HEAP_HISTORY_SIZE - 1; i++) {
        int older = (s_heap_idx + i) % HEAP_HISTORY_SIZE;
        int newer = (s_heap_idx + i + 1) % HEAP_HISTORY_SIZE;
        if (s_heap_history[newer] >= s_heap_history[older]) {
            declining = false;
            break;
        }
    }
    if (declining && !s_heap_alert_sent) {
        s_heap_alert_sent = true;
        ESP_LOGE(TAG, "Possible heap leak: %lu -> %lu over %d samples",
                 (unsigned long)s_heap_history[(s_heap_idx) % HEAP_HISTORY_SIZE],
                 (unsigned long)s_heap_history[(s_heap_idx + HEAP_HISTORY_SIZE - 1) % HEAP_HISTORY_SIZE],
                 HEAP_HISTORY_SIZE);
        /* Send MQTT alert */
        if (mqtt_client_is_connected() && device_registration_is_registered()) {
            char topic[96], payload[128];
            snprintf(topic, sizeof(topic), "devices/%s/alert",
                     device_registration_get_id());
            snprintf(payload, sizeof(payload),
                     "{\"type\":\"heap_leak\",\"free_heap\":%lu,\"min_heap\":%lu}",
                     (unsigned long)free_heap,
                     (unsigned long)esp_get_minimum_free_heap_size());
            mqtt_client_publish(topic, payload, 1);
        }
    } else if (!declining) {
        s_heap_alert_sent = false;
    }
}

void status_reporter_tick(const char *device_id)
{
    if (!s_initialized) {
        s_boot_time_us = esp_timer_get_time();
        s_initialized = true;
    }

    int64_t now = esp_timer_get_time();
    if ((now - s_last_report_us) < (HEARTBEAT_INTERVAL_MS * 1000LL)) {
        return;
    }
    s_last_report_us = now;

    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();

    heap_monitor_update(free_heap);

    if (min_heap < 4096) {
        ESP_LOGW(TAG, "Low heap: free=%lu min=%lu", (unsigned long)free_heap, (unsigned long)min_heap);
        /* Critical low heap alert (once) */
        static bool s_low_heap_alerted = false;
        if (!s_low_heap_alerted && mqtt_client_is_connected() && device_registration_is_registered()) {
            s_low_heap_alerted = true;
            char topic[96], payload[96];
            snprintf(topic, sizeof(topic), "devices/%s/alert",
                     device_registration_get_id());
            snprintf(payload, sizeof(payload),
                     "{\"type\":\"low_heap\",\"free\":%lu,\"min\":%lu}",
                     (unsigned long)free_heap, (unsigned long)min_heap);
            mqtt_client_publish(topic, payload, 1);
        }
    }

    if (!mqtt_client_is_connected()) return;

    int8_t rssi = wifi_manager_get_rssi();
    uint32_t uptime_sec = (uint32_t)((now - s_boot_time_us) / 1000000LL);
    mqtt_client_send_heartbeat(device_id, rssi, uptime_sec);
}
