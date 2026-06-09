#include "device_registration.h"
#include "nvs_storage.h"
#include "config.h"
#include "ca_cert.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "dev_reg";

static char s_device_id[64] = {0};
static char s_mqtt_user[64] = {0};
static char s_mqtt_pass[128] = {0};
static bool s_registered = false;

/* HTTP response buffer */
static char s_response_buf[2048];
static int s_response_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy_len = evt->data_len;
        if (s_response_len + copy_len < (int)sizeof(s_response_buf)) {
            memcpy(s_response_buf + s_response_len, evt->data, copy_len);
            s_response_len += copy_len;
        }
    }
    return ESP_OK;
}

bool device_registration_is_registered(void)
{
    if (s_registered) return true;

    esp_err_t err = nvs_storage_get_str(NVS_NAMESPACE_DEVICE, "id", s_device_id, sizeof(s_device_id));
    if (err == ESP_OK && strlen(s_device_id) > 0) {
        nvs_storage_get_str(NVS_NAMESPACE_MQTT, "user", s_mqtt_user, sizeof(s_mqtt_user));
        nvs_storage_get_str(NVS_NAMESPACE_MQTT, "pass", s_mqtt_pass, sizeof(s_mqtt_pass));
        s_registered = true;
        return true;
    }
    return false;
}

esp_err_t device_registration_register(void)
{
    /* Get MAC address as unique identifier */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Build request body */
    char post_data[128];
    snprintf(post_data, sizeof(post_data),
             "{\"device_id\":\"%s\",\"firmware_version\":\"%s\"}", mac_str, FW_VERSION);

    char url[192];
    snprintf(url, sizeof(url), "%s%s", API_BASE_URL, API_REGISTER_PATH);

    s_response_len = 0;
    memset(s_response_buf, 0, sizeof(s_response_buf));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .cert_pem = ca_cert_pem,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || (status != 200 && status != 201)) {
        ESP_LOGE(TAG, "Registration failed: err=%s, status=%d", esp_err_to_name(err), status);
        return ESP_FAIL;
    }

    /* Null-terminate response buffer */
    s_response_buf[s_response_len] = '\0';

    /* Parse response */
    cJSON *root = cJSON_Parse(s_response_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse registration response");
        return ESP_FAIL;
    }

    /* Check registration status */
    cJSON *status_obj = cJSON_GetObjectItem(root, "status");
    if (status_obj && cJSON_IsString(status_obj) && strcmp(status_obj->valuestring, "pending") == 0) {
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Device pending approval");
        return ESP_FAIL;
    }

    /* Credentials are nested under "credentials" key */
    cJSON *creds = cJSON_GetObjectItem(root, "credentials");
    if (!creds) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "No credentials in response (device may be pending)");
        return ESP_FAIL;
    }

    cJSON *id = cJSON_GetObjectItem(root, "device_id");
    cJSON *mqtt_u = cJSON_GetObjectItem(creds, "mqtt_username");
    cJSON *mqtt_p = cJSON_GetObjectItem(creds, "mqtt_password");

    if (!id || !mqtt_u || !mqtt_p) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Missing fields in registration response");
        return ESP_FAIL;
    }

    /* Check if password is masked (device needs credential regeneration) */
    if (strcmp(mqtt_p->valuestring, "***") == 0) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Credentials masked — admin must regenerate MQTT password");
        return ESP_FAIL;
    }

    strncpy(s_device_id, id->valuestring, sizeof(s_device_id) - 1);
    s_device_id[sizeof(s_device_id) - 1] = '\0';
    strncpy(s_mqtt_user, mqtt_u->valuestring, sizeof(s_mqtt_user) - 1);
    s_mqtt_user[sizeof(s_mqtt_user) - 1] = '\0';
    strncpy(s_mqtt_pass, mqtt_p->valuestring, sizeof(s_mqtt_pass) - 1);
    s_mqtt_pass[sizeof(s_mqtt_pass) - 1] = '\0';
    cJSON_Delete(root);

    /* Save to NVS */
    nvs_storage_set_str(NVS_NAMESPACE_DEVICE, "id", s_device_id);
    nvs_storage_set_str(NVS_NAMESPACE_MQTT, "user", s_mqtt_user);
    nvs_storage_set_str(NVS_NAMESPACE_MQTT, "pass", s_mqtt_pass);

    s_registered = true;
    ESP_LOGI(TAG, "Device registered: id=%s", s_device_id);
    return ESP_OK;
}

const char *device_registration_get_id(void) { return s_device_id; }
const char *device_registration_get_mqtt_user(void) { return s_mqtt_user; }
const char *device_registration_get_mqtt_pass(void) { return s_mqtt_pass; }

esp_err_t device_registration_register_with_retry(void)
{
    /* Exponential backoff: 5s, 15s, 45s */
    const uint32_t delays_ms[] = {5000, 15000, 45000};
    const int max_attempts = 3;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        esp_err_t err = device_registration_register();
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (attempt < max_attempts - 1) {
            ESP_LOGW(TAG, "Registration attempt %d failed, retrying in %lums",
                     attempt + 1, (unsigned long)delays_ms[attempt]);
            vTaskDelay(pdMS_TO_TICKS(delays_ms[attempt]));
        }
    }
    ESP_LOGE(TAG, "Registration failed after %d attempts", max_attempts);
    return ESP_FAIL;
}

esp_err_t device_registration_fetch_credentials(const char *api_key)
{
    if (!api_key || strlen(api_key) == 0) {
        ESP_LOGE(TAG, "API key is empty");
        return ESP_ERR_INVALID_ARG;
    }

    char post_data[128];
    snprintf(post_data, sizeof(post_data), "{\"api_key\":\"%s\"}", api_key);

    char url[192];
    snprintf(url, sizeof(url), "%s%s", API_BASE_URL, API_CREDENTIALS_PATH);

    s_response_len = 0;
    memset(s_response_buf, 0, sizeof(s_response_buf));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .cert_pem = ca_cert_pem,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client for credentials");
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || http_status != 200) {
        ESP_LOGE(TAG, "Credentials fetch failed: err=%s, status=%d", esp_err_to_name(err), http_status);
        return ESP_FAIL;
    }

    s_response_buf[s_response_len] = '\0';

    cJSON *root = cJSON_Parse(s_response_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse credentials response");
        return ESP_FAIL;
    }

    cJSON *mqtt_u = cJSON_GetObjectItem(root, "mqtt_username");
    cJSON *mqtt_p = cJSON_GetObjectItem(root, "mqtt_password");
    cJSON *id = cJSON_GetObjectItem(root, "device_id");

    if (!mqtt_u || !mqtt_p) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Missing MQTT credentials in response");
        return ESP_FAIL;
    }

    if (id && cJSON_IsString(id)) {
        strncpy(s_device_id, id->valuestring, sizeof(s_device_id) - 1);
        s_device_id[sizeof(s_device_id) - 1] = '\0';
    }
    strncpy(s_mqtt_user, mqtt_u->valuestring, sizeof(s_mqtt_user) - 1);
    s_mqtt_user[sizeof(s_mqtt_user) - 1] = '\0';
    strncpy(s_mqtt_pass, mqtt_p->valuestring, sizeof(s_mqtt_pass) - 1);
    s_mqtt_pass[sizeof(s_mqtt_pass) - 1] = '\0';
    cJSON_Delete(root);

    nvs_storage_set_str(NVS_NAMESPACE_DEVICE, "id", s_device_id);
    nvs_storage_set_str(NVS_NAMESPACE_MQTT, "user", s_mqtt_user);
    nvs_storage_set_str(NVS_NAMESPACE_MQTT, "pass", s_mqtt_pass);

    s_registered = true;
    ESP_LOGI(TAG, "Credentials fetched successfully for device: %s", s_device_id);
    return ESP_OK;
}
