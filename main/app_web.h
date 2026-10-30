#pragma once
#include "esp_err.h"

/**
 * Built-in configuration web server (port 80), serving an embedded page:
 *
 *   GET  /             - configuration page (embedded HTML)
 *   GET  /config.json  - current configuration ( passwords never exposed,
 *                        only "pw_set" / "pass_set" flags) incl. versions,
 *                        update URLs and the online update-check state
 *   POST /save         - applies the form and reboots the device
 *   POST /ota/install  - installs the firmware from the ACTIVE URL
 *                        (manual web-UI URL, else CONFIG_JKBMS_OTA_DEFAULT_URL)
 *   GET  /ota/status   - last OTA status text
 *   POST /ota/check    - starts an immediate online update check
 *                        (fetch the remote "version" file, compare)
 *   GET  /ota/version  - JSON snapshot: {installed, remote, available,
 *                        checked, checking}
 *
 * All handlers require HTTP Basic auth when a web password is configured;
 * username is ignored, only the password is compared.
 */
esp_err_t app_web_start(void);   /* call once an IP address is available */
void app_web_stop(void);         /* on link loss (httpd_stop) */
