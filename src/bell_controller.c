#include "bell_controller.h"
#include "config.h"
#include "nvs_storage.h"
#include "entitlement_manager.h"
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "hal/gpio_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bell";
static SemaphoreHandle_t s_ring_mutex = NULL;

/* Offline bell log queue — stores up to 20 events in NVS for upload on reconnect */
#define OFFLINE_LOG_MAX 20

typedef struct __attribute__((packed)) {
    time_t timestamp;
    uint32_t duration_ms;
    char source[12]; /* "schedule", "manual", "emergency", "fire_alarm" */
} offline_bell_log_t;

static offline_bell_log_t s_offline_log[OFFLINE_LOG_MAX];
static uint8_t s_offline_log_count = 0;

typedef struct {
    uint32_t duration_ms;
    char source[16];
} ring_task_arg_t;

static void offline_log_save(time_t ts, uint32_t duration_ms, const char *source)
{
    if (s_offline_log_count >= OFFLINE_LOG_MAX) {
        /* Shift out oldest entry */
        memmove(&s_offline_log[0], &s_offline_log[1],
                (OFFLINE_LOG_MAX - 1) * sizeof(offline_bell_log_t));
        s_offline_log_count = OFFLINE_LOG_MAX - 1;
    }
    s_offline_log[s_offline_log_count].timestamp = ts;
    s_offline_log[s_offline_log_count].duration_ms = duration_ms;
    strlcpy(s_offline_log[s_offline_log_count].source,
            source ? source : "schedule",
            sizeof(s_offline_log[s_offline_log_count].source));
    s_offline_log_count++;
    /* Persist to NVS */
    nvs_storage_set_blob(NVS_NAMESPACE_CONFIG, "blog", s_offline_log,
                         s_offline_log_count * sizeof(offline_bell_log_t));
    uint8_t cnt = s_offline_log_count;
    nvs_storage_set_blob(NVS_NAMESPACE_CONFIG, "blogcnt", &cnt, sizeof(cnt));
}

void bell_controller_load_offline_log(void)
{
    size_t cnt_len = sizeof(s_offline_log_count);
    if (nvs_storage_get_blob(NVS_NAMESPACE_CONFIG, "blogcnt", &s_offline_log_count, &cnt_len) != ESP_OK) {
        s_offline_log_count = 0;
        return;
    }
    if (s_offline_log_count > OFFLINE_LOG_MAX) s_offline_log_count = OFFLINE_LOG_MAX;
    if (s_offline_log_count > 0) {
        size_t log_len = s_offline_log_count * sizeof(offline_bell_log_t);
        nvs_storage_get_blob(NVS_NAMESPACE_CONFIG, "blog", s_offline_log, &log_len);
        ESP_LOGI(TAG, "Loaded %d offline bell logs from NVS", s_offline_log_count);
    }
}

void bell_controller_flush_offline_log(void)
{
    if (s_offline_log_count == 0) return;

    extern const char *device_registration_get_id(void);
    extern esp_err_t mqtt_client_publish(const char *, const char *, int);
    extern bool mqtt_client_is_connected(void);

    if (!mqtt_client_is_connected()) return;

    const char *dev_id = device_registration_get_id();
    if (!dev_id || !dev_id[0]) return;

    char topic[96];
    snprintf(topic, sizeof(topic), "devices/%s/bell_log", dev_id);

    for (int i = 0; i < s_offline_log_count; i++) {
        struct tm t;
        time_t ts = s_offline_log[i].timestamp;
        localtime_r(&ts, &t);
        const char *src = s_offline_log[i].source[0] ? s_offline_log[i].source : "offline";
        char payload[176];
        snprintf(payload, sizeof(payload),
                 "{\"time\":\"%04d-%02d-%02dT%02d:%02d:%02d\",\"duration\":%lu,\"source\":\"%s\",\"offline\":true}",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec,
                 (unsigned long)s_offline_log[i].duration_ms, src);
        mqtt_client_publish(topic, payload, 1);
    }
    ESP_LOGI(TAG, "Flushed %d offline bell logs", s_offline_log_count);
    s_offline_log_count = 0;
    /* Clear from NVS */
    uint8_t zero = 0;
    nvs_storage_set_blob(NVS_NAMESPACE_CONFIG, "blogcnt", &zero, sizeof(zero));
}

esp_err_t bell_controller_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BELL_GPIO_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    gpio_set_level(BELL_GPIO_PIN, 1); /* active-low relay OFF */
    s_ring_mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(s_ring_mutex);
    ESP_LOGI(TAG, "Bell GPIO %d initialized", BELL_GPIO_PIN);
    return err;
}

static void ring_task(void *arg)
{
    ring_task_arg_t task_arg = {0};
    if (arg) {
        task_arg = *(ring_task_arg_t *)arg;
        free(arg);
    }
    uint32_t duration_ms = task_arg.duration_ms;
    const char *source = task_arg.source[0] ? task_arg.source : "schedule";
    gpio_set_level(BELL_GPIO_PIN, 0); /* active-low relay ON */
    ESP_LOGI(TAG, "Bell ON for %lu ms", (unsigned long)duration_ms);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(BELL_GPIO_PIN, 1); /* active-low relay OFF */
    ESP_LOGI(TAG, "Bell OFF");

    /* Log bell ring — send via MQTT if online, store in NVS if offline */
    extern const char *device_registration_get_id(void);
    extern esp_err_t mqtt_client_publish(const char *, const char *, int);
    extern bool mqtt_client_is_connected(void);

    time_t now; time(&now);
    const char *dev_id = device_registration_get_id();

    if (dev_id && dev_id[0] && mqtt_client_is_connected()) {
        char topic[96], payload[160];
        struct tm t; localtime_r(&now, &t);
        snprintf(topic, sizeof(topic), "devices/%s/bell_log", dev_id);
        snprintf(payload, sizeof(payload),
            "{\"time\":\"%04d-%02d-%02dT%02d:%02d:%02d\",\"duration\":%lu,\"source\":\"%s\"}",
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
            t.tm_hour, t.tm_min, t.tm_sec, (unsigned long)duration_ms, source);
        mqtt_client_publish(topic, payload, 1);
    } else {
        /* Offline — save to NVS for later upload (with original source) */
        offline_log_save(now, duration_ms, source);
        ESP_LOGI(TAG, "Bell log saved offline (%d queued)", s_offline_log_count);
    }

    xSemaphoreGive(s_ring_mutex);
    vTaskDelete(NULL);
}

esp_err_t bell_controller_ring(uint32_t duration_ms)
{
    return bell_controller_ring_with_source(duration_ms, "schedule");
}

esp_err_t bell_controller_ring_with_source(uint32_t duration_ms, const char *source)
{
    const char *ring_source = (source && source[0]) ? source : "schedule";
    if (!entitlement_manager_can_ring(ring_source)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duration_ms == 0) {
        duration_ms = BELL_DEFAULT_DURATION_MS;
    }
    if (duration_ms > BELL_MAX_DURATION_MS) {
        ESP_LOGW(TAG, "Duration %lu ms exceeds cap, clamping to %d ms",
                 (unsigned long)duration_ms, BELL_MAX_DURATION_MS);
        duration_ms = BELL_MAX_DURATION_MS;
    }
    if (xSemaphoreTake(s_ring_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Bell already ringing, ignoring");
        return ESP_ERR_INVALID_STATE;
    }

    ring_task_arg_t *arg = calloc(1, sizeof(ring_task_arg_t));
    if (!arg) {
        xSemaphoreGive(s_ring_mutex);
        return ESP_ERR_NO_MEM;
    }
    arg->duration_ms = duration_ms;
    strlcpy(arg->source, ring_source, sizeof(arg->source));

    BaseType_t ret = xTaskCreate(ring_task, "bell_ring", 4096,
                                 arg, 5, NULL);
    if (ret != pdPASS) {
        free(arg);
        xSemaphoreGive(s_ring_mutex);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void bell_controller_stop(void)
{
    gpio_set_level(BELL_GPIO_PIN, 1); /* active-low relay OFF */
}

void bell_controller_panic_off(void)
{
    /* Direct register write — safe to call from panic context (no RTOS) */
    gpio_ll_set_level(&GPIO, BELL_GPIO_PIN, 1);
}
