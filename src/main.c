#include "config.h"
#include "ap_provisioning.h"
#include "nvs_storage.h"
#include "wifi_manager.h"
#include "app_mqtt.h"
#include "bell_controller.h"
#include "schedule_manager.h"
#include "holiday_manager.h"
#include "rtc_diagnostics.h"
#include "device_registration.h"
#include "status_reporter.h"
#include "rtc_driver.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "main";

#define WDT_TIMEOUT_SEC 30

static void shutdown_handler(void)
{
    bell_controller_panic_off();
}

static void apply_timezone(void)
{
    char tz[64] = {0};
    if (nvs_storage_get_str(NVS_NAMESPACE_CONFIG, "tz", tz, sizeof(tz)) != ESP_OK
        || strlen(tz) == 0) {
        strncpy(tz, DEFAULT_TZ_STRING, sizeof(tz) - 1);
    }
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", tz);
}

static volatile bool s_sntp_synced = false;

static void sntp_sync_cb(struct timeval *tv)
{
    s_sntp_synced = true;
    ESP_LOGI(TAG, "SNTP callback: time synced");
}

static bool sntp_sync_once(void)
{
    s_sntp_synced = false;
    int retry = 0;
    while (!s_sntp_synced && retry < 40) { /* max 20s */
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }
    return s_sntp_synced;
}

static void init_sntp(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP initialized, waiting for sync...");

    if (sntp_sync_once()) {
        ESP_LOGI(TAG, "SNTP time synced");
        rtc_driver_save_from_system(); /* Save to RTC hardware */
    } else {
        ESP_LOGW(TAG, "SNTP sync timeout, using RTC time");
    }
}

static void sntp_periodic_resync(uint32_t *last_sync_tick)
{
    uint32_t now_tick = xTaskGetTickCount();
    uint32_t interval = pdMS_TO_TICKS(SNTP_RESYNC_INTERVAL_MS);

    if ((now_tick - *last_sync_tick) < interval) return;

    ESP_LOGI(TAG, "Periodic SNTP re-sync...");
    esp_sntp_restart();

    if (sntp_sync_once()) {
        ESP_LOGI(TAG, "SNTP re-sync OK");
        rtc_driver_save_from_system();
        *last_sync_tick = now_tick;
    } else {
        ESP_LOGW(TAG, "SNTP re-sync failed, retry in %d ms", SNTP_RETRY_INTERVAL_MS);
        /* Retry sooner on failure */
        *last_sync_tick = now_tick - interval + pdMS_TO_TICKS(SNTP_RETRY_INTERVAL_MS);
    }
}

static void mark_ota_valid_after_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "OTA rollback: marking firmware as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
}

static void wifi_fallback_handler(void)
{
    ESP_LOGW(TAG, "WiFi failed — starting AP+STA provisioning mode");
    wifi_manager_start_apsta();
    ap_provisioning_start_portal_only();
}

void app_main(void)
{
    ESP_LOGI(TAG, "School Device Firmware v%s starting...", FW_VERSION);

    /* 0. Initialize watchdog timer */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = WDT_TIMEOUT_SEC * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_task_wdt_init(&wdt_cfg);
    esp_task_wdt_add(NULL);

    /* 1. Initialize NVS */
    ESP_ERROR_CHECK(nvs_storage_init());

    /* Register shutdown handler to turn off bell on panic/restart */
    esp_register_shutdown_handler(shutdown_handler);

    /* 2. Initialize RTC (DS1307) — set system time from hardware clock */
    apply_timezone();

    /* 3. Apply timezone from NVS (or default) */
    rtc_driver_init();

    /* 3. Initialize bell GPIO */
    ESP_ERROR_CHECK(bell_controller_init());

    /* 4. Connect to WiFi */
    esp_err_t wifi_err = wifi_manager_init_with_fallback(wifi_fallback_handler);
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi failed, running offline with cached schedule");
    }

    /* 5. Load schedule and holidays from NVS (works offline) */
    schedule_manager_init();
    holiday_manager_init();
    rtc_diagnostics_init();

    /* 6. If online: sync time, register device, connect MQTT */
    uint32_t sntp_last_sync = 0;
    if (wifi_manager_is_connected()) {
        init_sntp();
        sntp_last_sync = xTaskGetTickCount();

        if (!device_registration_is_registered()) {
            ESP_LOGI(TAG, "First boot — registering device...");
            if (device_registration_register_with_retry() != ESP_OK) {
                ESP_LOGE(TAG, "Registration failed after retries");
            }
        }

        /* Mark OTA valid after WiFi connects — do NOT depend on MQTT */
        mark_ota_valid_after_boot();

        if (device_registration_is_registered()) {
            const char *dev_id = device_registration_get_id();
            mqtt_client_init(dev_id,
                             device_registration_get_mqtt_user(),
                             device_registration_get_mqtt_pass());
        }
    }

    /* 7. Main loop */
    bool was_connected = wifi_manager_is_connected();

    while (1) {
        esp_task_wdt_reset();

        schedule_manager_tick();
        rtc_diagnostics_tick();

        if (device_registration_is_registered()) {
            status_reporter_tick(device_registration_get_id());
        }

        /* Periodic SNTP re-sync (every hour) */
        if (wifi_manager_is_connected() && sntp_last_sync > 0) {
            sntp_periodic_resync(&sntp_last_sync);
        }

        /* WiFi reconnection monitoring */
        bool now_connected = wifi_manager_is_connected();
        if (was_connected && !now_connected) {
            ESP_LOGW(TAG, "WiFi lost");
            sntp_last_sync = 0;  /* Force SNTP re-init on reconnect */
        }
        if (now_connected && !was_connected) {
            ESP_LOGI(TAG, "WiFi restored");
            if (sntp_last_sync == 0) {
                init_sntp();
                sntp_last_sync = xTaskGetTickCount();
            }
            if (device_registration_is_registered() && !mqtt_client_is_connected()) {
                const char *dev_id = device_registration_get_id();
                mqtt_client_init(dev_id,
                                 device_registration_get_mqtt_user(),
                                 device_registration_get_mqtt_pass());
            }
        }
        was_connected = now_connected;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
