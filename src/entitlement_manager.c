#include "entitlement_manager.h"
#include "config.h"
#include "ca_cert.h"
#include "nvs_storage.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "entitlement";

typedef struct __attribute__((packed)) {
    uint32_t version;
    time_t valid_until;
    time_t grace_until;
    time_t last_known_time;
    time_t last_sync_at;
    uint8_t active;
} entitlement_record_t;

static entitlement_record_t s_entitlement = {0};
static entitlement_state_t s_state = ENTITLEMENT_STATE_UNKNOWN;
static char s_response_buf[2048];
static int s_response_len = 0;
static char s_last_error[64] = "";

static void set_last_error(const char *error)
{
    if (!error) {
        s_last_error[0] = '\0';
        return;
    }
    strlcpy(s_last_error, error, sizeof(s_last_error));
}

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

static bool constant_time_eq(const unsigned char *a, const unsigned char *b, size_t len)
{
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

static esp_err_t b64url_decode(const char *input, unsigned char *out, size_t out_size, size_t *out_len)
{
    size_t in_len = strlen(input);
    size_t padded_len = in_len + ((4 - (in_len % 4)) % 4);
    char *tmp = calloc(1, padded_len + 1);
    if (!tmp) return ESP_ERR_NO_MEM;

    for (size_t i = 0; i < in_len; i++) {
        if (input[i] == '-') tmp[i] = '+';
        else if (input[i] == '_') tmp[i] = '/';
        else tmp[i] = input[i];
    }
    for (size_t i = in_len; i < padded_len; i++) {
        tmp[i] = '=';
    }

    int ret = mbedtls_base64_decode(out, out_size, out_len, (const unsigned char *)tmp, padded_len);
    free(tmp);
    return ret == 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t verify_and_apply_token(const char *token)
{
    const char *dot = strchr(token, '.');
    if (!dot || strchr(dot + 1, '.')) {
        set_last_error("bad_token_format");
        return ESP_ERR_INVALID_ARG;
    }

    size_t payload_part_len = dot - token;
    char payload_part[1536] = {0};
    char sig_part[96] = {0};
    if (payload_part_len >= sizeof(payload_part) || strlen(dot + 1) >= sizeof(sig_part)) {
        set_last_error("token_too_large");
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(payload_part, token, payload_part_len);
    strlcpy(sig_part, dot + 1, sizeof(sig_part));

    unsigned char payload[1024] = {0};
    unsigned char signature[32] = {0};
    size_t payload_len = 0;
    size_t sig_len = 0;
    if (b64url_decode(payload_part, payload, sizeof(payload) - 1, &payload_len) != ESP_OK ||
        b64url_decode(sig_part, signature, sizeof(signature), &sig_len) != ESP_OK ||
        sig_len != 32) {
        set_last_error("token_decode_failed");
        return ESP_ERR_INVALID_ARG;
    }
    payload[payload_len] = '\0';

    unsigned char expected[32] = {0};
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) return ESP_FAIL;
    int hmac_ret = mbedtls_md_hmac(
        md_info,
        (const unsigned char *)ENTITLEMENT_HMAC_SECRET,
        strlen(ENTITLEMENT_HMAC_SECRET),
        payload,
        payload_len,
        expected
    );
    if (hmac_ret != 0 || !constant_time_eq(signature, expected, sizeof(expected))) {
        ESP_LOGE(TAG, "Entitlement signature invalid");
        set_last_error("bad_signature");
        return ESP_ERR_INVALID_CRC;
    }

    cJSON *root = cJSON_Parse((const char *)payload);
    if (!root) {
        set_last_error("bad_payload_json");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *status = cJSON_GetObjectItem(root, "status");
    cJSON *valid_until = cJSON_GetObjectItem(root, "valid_until");
    cJSON *grace_until = cJSON_GetObjectItem(root, "grace_until");
    cJSON *version = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsString(status) || !cJSON_IsNumber(valid_until) ||
        !cJSON_IsNumber(grace_until) || !cJSON_IsNumber(version)) {
        cJSON_Delete(root);
        set_last_error("bad_payload_fields");
        return ESP_ERR_INVALID_ARG;
    }

    s_entitlement.active = strcmp(status->valuestring, "active") == 0;
    s_entitlement.valid_until = (time_t)valid_until->valuedouble;
    s_entitlement.grace_until = (time_t)grace_until->valuedouble;
    s_entitlement.version = (uint32_t)version->valueint;

    time_t now = time(NULL);
    if (now > 1577836800 && now > s_entitlement.last_known_time) {
        s_entitlement.last_known_time = now;
    }
    if (now > 1577836800) {
        s_entitlement.last_sync_at = now;
    }

    nvs_storage_set_blob(NVS_NAMESPACE_ENTITLEMENT, "record", &s_entitlement, sizeof(s_entitlement));
    nvs_storage_set_str(NVS_NAMESPACE_ENTITLEMENT, "token", token);
    ESP_LOGI(TAG, "Entitlement saved: active=%d valid_until=%ld grace_until=%ld version=%lu",
             s_entitlement.active, (long)s_entitlement.valid_until,
             (long)s_entitlement.grace_until, (unsigned long)s_entitlement.version);
    set_last_error(NULL);
    cJSON_Delete(root);
    return ESP_OK;
}

/* Upper bound for "reasonable" current time — year 2100.
 * Prevents garbage RTC time (e.g. DS1302 cold-boot year 2167) from
 * polluting last_known_time and triggering false "time moved backwards". */
#define ENTITLEMENT_MAX_REAL_TIME ((time_t)4102444800LL)

static entitlement_state_t compute_state(void)
{
#if ENTITLEMENT_REQUIRED == 0
    if (s_entitlement.version == 0) return ENTITLEMENT_STATE_ACTIVE;
#endif
    if (s_entitlement.version == 0) return ENTITLEMENT_STATE_NO_TOKEN;
    if (!s_entitlement.active) return ENTITLEMENT_STATE_EXPIRED;

    time_t now = time(NULL);
    if (now < 1577836800) return ENTITLEMENT_STATE_INVALID_TIME;

    /* Only check backwards-time if last_known_time itself is a realistic timestamp.
     * Garbage RTC time (year 2167) > ENTITLEMENT_MAX_REAL_TIME — ignore it. */
    if (s_entitlement.last_known_time > 0
        && s_entitlement.last_known_time < ENTITLEMENT_MAX_REAL_TIME
        && now + 600 < s_entitlement.last_known_time) {
        ESP_LOGW(TAG, "Time moved backwards; blocking subscription-gated actions");
        return ENTITLEMENT_STATE_INVALID_TIME;
    }
    /* Only update last_known_time with a realistic current timestamp */
    if (now > 1577836800 && now < ENTITLEMENT_MAX_REAL_TIME
        && now > s_entitlement.last_known_time) {
        s_entitlement.last_known_time = now;
        nvs_storage_set_blob(NVS_NAMESPACE_ENTITLEMENT, "record", &s_entitlement, sizeof(s_entitlement));
    }
    if (now <= s_entitlement.valid_until) return ENTITLEMENT_STATE_ACTIVE;
    if (now <= s_entitlement.grace_until) return ENTITLEMENT_STATE_GRACE;
    return ENTITLEMENT_STATE_EXPIRED;
}

esp_err_t entitlement_manager_init(void)
{
    size_t len = sizeof(s_entitlement);
    if (nvs_storage_get_blob(NVS_NAMESPACE_ENTITLEMENT, "record", &s_entitlement, &len) != ESP_OK ||
        len != sizeof(s_entitlement)) {
        memset(&s_entitlement, 0, sizeof(s_entitlement));
        ESP_LOGW(TAG, "No entitlement in NVS");
    }
    /* Clear garbage last_known_time (e.g. from DS1302 cold-boot year 2167) */
    if (s_entitlement.last_known_time >= ENTITLEMENT_MAX_REAL_TIME) {
        s_entitlement.last_known_time = 0;
    }
    s_state = compute_state();
    ESP_LOGI(TAG, "Entitlement state=%d version=%lu", s_state, (unsigned long)s_entitlement.version);
    return ESP_OK;
}

esp_err_t entitlement_manager_fetch(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        set_last_error("missing_device_id");
        return ESP_ERR_INVALID_ARG;
    }

    char post_data[96];
    snprintf(post_data, sizeof(post_data), "{\"device_id\":\"%s\"}", device_id);

    char url[192];
    snprintf(url, sizeof(url), "%s%s", API_BASE_URL, API_ENTITLEMENT_PATH);

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
    if (!client) {
        set_last_error("http_init_failed");
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Entitlement fetch failed: err=%s status=%d", esp_err_to_name(err), status);
        set_last_error(status == 200 ? "fetch_failed" : "http_status");
        return ESP_FAIL;
    }

    s_response_buf[s_response_len] = '\0';
    cJSON *root = cJSON_Parse(s_response_buf);
    if (!root) {
        set_last_error("bad_response_json");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *token = cJSON_GetObjectItem(root, "token");
    if (!cJSON_IsString(token)) {
        cJSON_Delete(root);
        set_last_error("missing_token");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t apply_err = verify_and_apply_token(token->valuestring);
    cJSON_Delete(root);
    s_state = compute_state();
    return apply_err;
}

bool entitlement_manager_can_ring(const char *source)
{
    if (source && (strcmp(source, "emergency") == 0 || strcmp(source, "fire_alarm") == 0)) {
        return true;
    }
    s_state = compute_state();
    if (s_state == ENTITLEMENT_STATE_ACTIVE || s_state == ENTITLEMENT_STATE_GRACE) {
        return true;
    }
    ESP_LOGW(TAG, "Ring blocked by subscription state=%d", s_state);
    return false;
}

entitlement_state_t entitlement_manager_get_state(void)
{
    s_state = compute_state();
    return s_state;
}

const char *entitlement_manager_get_state_string(void)
{
    switch (entitlement_manager_get_state()) {
        case ENTITLEMENT_STATE_ACTIVE:
            return "active";
        case ENTITLEMENT_STATE_GRACE:
            return "grace";
        case ENTITLEMENT_STATE_EXPIRED:
            return "expired";
        case ENTITLEMENT_STATE_INVALID_TIME:
            return "invalid_time";
        case ENTITLEMENT_STATE_NO_TOKEN:
            return "no_token";
        case ENTITLEMENT_STATE_UNKNOWN:
        default:
            return "unknown";
    }
}

uint32_t entitlement_manager_get_version(void)
{
    return s_entitlement.version;
}

time_t entitlement_manager_get_valid_until(void)
{
    return s_entitlement.valid_until;
}

time_t entitlement_manager_get_grace_until(void)
{
    return s_entitlement.grace_until;
}

time_t entitlement_manager_get_last_sync_at(void)
{
    return s_entitlement.last_sync_at;
}

const char *entitlement_manager_get_last_error(void)
{
    return s_last_error;
}
