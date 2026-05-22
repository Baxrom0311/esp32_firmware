#include "status_reporter.h"
#include "app_mqtt.h"
#include "wifi_manager.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char *TAG = "status";
static int64_t s_last_report_us = 0;
static int64_t s_boot_time_us = 0;
static bool s_initialized = false;

/* Heap monitoring: track samples to detect leaks */
#define HEAP_HISTORY_SIZE 5
static uint32_t s_heap_history[HEAP_HISTORY_SIZE];
static int s_heap_idx = 0;
static bool s_heap_full = false;

static void heap_monitor_update(uint32_t free_heap)
{
    s_heap_history[s_heap_idx] = free_heap;
    s_heap_idx = (s_heap_idx + 1) % HEAP_HISTORY_SIZE;
    if (s_heap_idx == 0) s_heap_full = true;

    if (!s_heap_full) return;

    /* Check if heap is monotonically declining across all samples */
    bool declining = true;
    int prev = (s_heap_idx + HEAP_HISTORY_SIZE - 1) % HEAP_HISTORY_SIZE;
    for (int i = 1; i < HEAP_HISTORY_SIZE; i++) {
        int cur = (s_heap_idx + HEAP_HISTORY_SIZE - 1 - i) % HEAP_HISTORY_SIZE;
        if (s_heap_history[cur] >= s_heap_history[prev]) {
            declining = false;
            break;
        }
        prev = cur;
    }
    if (declining) {
        ESP_LOGE(TAG, "Possible heap leak: %lu -> %lu over %d samples",
                 (unsigned long)s_heap_history[(s_heap_idx) % HEAP_HISTORY_SIZE],
                 (unsigned long)s_heap_history[(s_heap_idx + HEAP_HISTORY_SIZE - 1) % HEAP_HISTORY_SIZE],
                 HEAP_HISTORY_SIZE);
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
    }

    if (!mqtt_client_is_connected()) return;

    int8_t rssi = wifi_manager_get_rssi();
    uint32_t uptime_sec = (uint32_t)((now - s_boot_time_us) / 1000000LL);
    mqtt_client_send_heartbeat(device_id, rssi, uptime_sec);
}
