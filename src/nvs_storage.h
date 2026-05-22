#ifndef NVS_STORAGE_H
#define NVS_STORAGE_H

#include "esp_err.h"

esp_err_t nvs_storage_init(void);
esp_err_t nvs_storage_get_str(const char *ns, const char *key, char *out, size_t max_len);
esp_err_t nvs_storage_set_str(const char *ns, const char *key, const char *value);
esp_err_t nvs_storage_get_blob(const char *ns, const char *key, void *out, size_t *len);
esp_err_t nvs_storage_set_blob(const char *ns, const char *key, const void *data, size_t len);
esp_err_t nvs_storage_erase_key(const char *ns, const char *key);
esp_err_t nvs_storage_erase_namespace(const char *ns);

#endif
