#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

#include "esp_err.h"

esp_err_t captive_dns_start(void);
void captive_dns_stop(void);

#endif
