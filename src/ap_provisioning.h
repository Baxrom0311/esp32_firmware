#ifndef AP_PROVISIONING_H
#define AP_PROVISIONING_H

#include "esp_err.h"
#include <stdbool.h>

/* Start full AP mode (creates SoftAP + HTTP + DNS) */
esp_err_t ap_provisioning_start(void);

/* Start portal only (HTTP + DNS) — use when AP+STA mode is already active */
esp_err_t ap_provisioning_start_portal_only(void);

void ap_provisioning_stop(void);
bool ap_provisioning_is_active(void);

#endif
