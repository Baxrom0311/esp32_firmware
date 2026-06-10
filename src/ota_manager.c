#include "ota_manager.h"
#include "app_mqtt.h"
#include "config.h"
#include "ca_cert.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ota";
static SemaphoreHandle_t s_ota_sem = NULL;

typedef struct {
    char url[256];
    char device_id[64];
} ota_task_params_t;

static bool validate_ota_host(const char *url)
{
    /* Require https:// prefix */
    if (strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "OTA URL must use HTTPS");
        return false;
    }

    const char *host_start = url + 8;
    const char *host_end = strchr(host_start, '/');
    if (!host_end) host_end = host_start + strlen(host_start);

    /* Strip port if present */
    const char *port = strchr(host_start, ':');
    if (port && port < host_end) host_end = port;

    size_t host_len = host_end - host_start;
    if (host_len == 0 || host_len > 253) return false;

    /* Exact match only — no subdomain spoofing */
    const char *allowed[] = { OTA_ALLOWED_HOST, OTA_ALLOWED_HOST_2 };
    for (int i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strlen(allowed[i]) == host_len &&
            strncmp(host_start, allowed[i], host_len) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t validate_image_header(const esp_app_desc_t *incoming)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_desc;
    esp_err_t err = esp_ota_get_partition_description(running, &running_desc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read running partition desc, skipping version check");
        return ESP_OK;
    }

    /* Semantic version comparison */
    int inc[3] = {0}, run[3] = {0};
    sscanf(incoming->version, "%d.%d.%d", &inc[0], &inc[1], &inc[2]);
    sscanf(running_desc.version, "%d.%d.%d", &run[0], &run[1], &run[2]);

    int cmp = 0;
    for (int i = 0; i < 3 && cmp == 0; i++) {
        cmp = inc[i] - run[i];
    }

    if (cmp == 0) {
        ESP_LOGW(TAG, "OTA image version same as running (%s), skipping", running_desc.version);
        return ESP_ERR_INVALID_VERSION;
    }
    if (cmp < 0) {
        ESP_LOGE(TAG, "OTA downgrade rejected: incoming '%s' < running '%s'",
                 incoming->version, running_desc.version);
        return ESP_ERR_INVALID_VERSION;
    }

    /* Verify project name matches to prevent flashing wrong firmware */
    if (memcmp(incoming->project_name, running_desc.project_name, sizeof(running_desc.project_name)) != 0) {
        ESP_LOGE(TAG, "OTA image project mismatch: got '%s', expected '%s'",
                 incoming->project_name, running_desc.project_name);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static void publish_ota_status(const char *device_id, const char *status, int progress)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "devices/%s/ota/status", device_id);
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"status\":\"%s\",\"progress\":%d}", status, progress);
    mqtt_client_publish(topic, payload, 1);
}

static void ota_task(void *arg)
{
    ota_task_params_t *params = (ota_task_params_t *)arg;

    ESP_LOGI(TAG, "Starting OTA from: %s", params->url);
    publish_ota_status(params->device_id, "downloading", 0);

    esp_http_client_config_t http_cfg = {
        .url = params->url,
        .timeout_ms = 30000,
        .cert_pem = ca_cert_pem,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        publish_ota_status(params->device_id, "failed", 0);
        goto cleanup;
    }

    /* Validate image header (project name + version) */
    esp_app_desc_t incoming_desc;
    err = esp_https_ota_get_img_desc(ota_handle, &incoming_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read OTA image desc: %s", esp_err_to_name(err));
        publish_ota_status(params->device_id, "failed", 0);
        esp_https_ota_abort(ota_handle);
        goto cleanup;
    }

    err = validate_image_header(&incoming_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA image validation failed: %s", esp_err_to_name(err));
        publish_ota_status(params->device_id, "failed", 0);
        esp_https_ota_abort(ota_handle);
        goto cleanup;
    }

    /* Download and flash */
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        int total = esp_https_ota_get_image_size(ota_handle);
        int read = esp_https_ota_get_image_len_read(ota_handle);
        if (total > 0) {
            int pct = (read * 100) / total;
            publish_ota_status(params->device_id, "downloading", pct);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        publish_ota_status(params->device_id, "failed", 0);
        esp_https_ota_abort(ota_handle);
        goto cleanup;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA success, rebooting...");
        publish_ota_status(params->device_id, "success", 100);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        publish_ota_status(params->device_id, "failed", 0);
    }

cleanup:
    free(params);
    xSemaphoreGive(s_ota_sem);
    vTaskDelete(NULL);
}

esp_err_t ota_manager_start(const char *firmware_url, const char *device_id)
{
    if (!firmware_url || strlen(firmware_url) < 9 ||
        strncmp(firmware_url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "Invalid OTA URL: must use HTTPS");
        return ESP_ERR_INVALID_ARG;
    }

    if (!validate_ota_host(firmware_url)) {
        ESP_LOGE(TAG, "OTA URL host not in whitelist");
        return ESP_ERR_INVALID_ARG;
    }

    /* Initialize semaphore on first use */
    if (s_ota_sem == NULL) {
        s_ota_sem = xSemaphoreCreateBinary();
        xSemaphoreGive(s_ota_sem);
    }

    /* Prevent concurrent OTA */
    if (xSemaphoreTake(s_ota_sem, 0) != pdTRUE) {
        ESP_LOGW(TAG, "OTA already in progress, rejecting");
        return ESP_ERR_INVALID_STATE;
    }

    ota_task_params_t *params = calloc(1, sizeof(ota_task_params_t));
    if (!params) {
        xSemaphoreGive(s_ota_sem);
        return ESP_ERR_NO_MEM;
    }

    strncpy(params->url, firmware_url, sizeof(params->url) - 1);
    strncpy(params->device_id, device_id, sizeof(params->device_id) - 1);

    BaseType_t ret = xTaskCreate(ota_task, "ota_task", 10240, params, 5, NULL);
    if (ret != pdPASS) {
        free(params);
        xSemaphoreGive(s_ota_sem);
        return ESP_FAIL;
    }
    return ESP_OK;
}
