#pragma once

#include "esp_err.h"

/* Start / stop a background task that answers every DNS A query with
 * the single IPv4 address `target_ip` (network byte order). */
esp_err_t dns_hijack_start(uint32_t target_ip_be);
void dns_hijack_stop(void);
