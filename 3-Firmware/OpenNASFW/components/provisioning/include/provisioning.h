#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/*
 * Register captive-portal HTTP handlers on `handle` and start the DNS
 * hijack task that redirects every DNS A query to the AP's IP. Call
 * only when running in NET_MODE_AP.
 */
esp_err_t provisioning_start(httpd_handle_t handle);

/* Stop the DNS task (not currently used — we just reboot on save). */
void provisioning_stop(void);
