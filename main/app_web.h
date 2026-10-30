#pragma once

#include "esp_err.h"

/**
 * Built-in configuration web server (port 80), serving an embedded page:
 *
 *   GET  /             - configuration page (embedded HTML)
 *   GET  /config.json  - current configuration ( passwords never exposed,
 *                        only "pw_set" / "pass_set" flags)
 *   POST /save         - applies the form and reboots the device
 *   POST /ota/install  - installs the firmware from CONFIG_JKBMS_OTA_DEFAULT_URL
 *   GET  /ota/status   - last OTA status text
 *
 * All handlers require HTTP Basic auth when a web password is configured;
 * username is ignored, only the password is compared.
 */
esp_err_t app_web_start(void);   /* call once an IP address is available */
void app_web_stop(void);         /* on link loss (httpd_stop) */
