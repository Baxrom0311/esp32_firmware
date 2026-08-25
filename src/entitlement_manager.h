#ifndef ENTITLEMENT_MANAGER_H
#define ENTITLEMENT_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    ENTITLEMENT_STATE_UNKNOWN = 0,
    ENTITLEMENT_STATE_ACTIVE,
    ENTITLEMENT_STATE_GRACE,
    ENTITLEMENT_STATE_EXPIRED,
    ENTITLEMENT_STATE_INVALID_TIME,
    ENTITLEMENT_STATE_NO_TOKEN,
} entitlement_state_t;

esp_err_t entitlement_manager_init(void);
esp_err_t entitlement_manager_fetch(const char *device_id);
bool entitlement_manager_can_ring(const char *source);
entitlement_state_t entitlement_manager_get_state(void);
const char *entitlement_manager_get_state_string(void);
uint32_t entitlement_manager_get_version(void);
time_t entitlement_manager_get_valid_until(void);
time_t entitlement_manager_get_grace_until(void);
time_t entitlement_manager_get_last_sync_at(void);
const char *entitlement_manager_get_last_error(void);

#endif
