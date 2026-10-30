#pragma once

#include <stdbool.h>

/**
 * Network firmware update (OTA), triggered via MQTT:
 *
 * Publishing an image URL ("http://..." or "https://...") as payload of
 * "<prefix>/ota" downloads the .bin into the INACTIVE OTA app slot and
 * reboots into it. Progress and the result are published to
 * "<prefix>/ota/status" (retained), e.g. "downloading 30%" or
 * "success, rebooting" / "failed: ...".
 *
 * Safety: with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the new image boots
 * in "pending verify" state and is only considered valid after
 * app_ota_confirm_running_image() was called on a healthy boot (MQTT up +
 * first successful BMS poll). Without this confirmation, the bootloader
 * automatically rolls back to the previous slot on the next reboot.
 *
 * http:// URLs are allowed (CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP); for
 * "https://..." the CA certificate embedded at build time (main/ca_cert.pem)
 * is used for server verification.
 */

/**
 * Starts the OTA download in an own task. Accepts only http(s) URLs and
 * rejects a second request while an update is already in progress.
 * Returns false on invalid URL or task-creation failure.
 */
bool app_ota_start_from_url(const char *url);

/**
 * Starts the OTA download from the build-time default URL
 * (CONFIG_JKBMS_OTA_DEFAULT_URL). Returns false if no default URL was
 * compiled in or the request is rejected (busy).
 */
bool app_ota_start_default(void);

/**
 * Last reachable OTA status text ("idle", "downloading 30%", "success,
 * rebooting", "failed: ...") - the same string that was last published to
 * "<prefix>/ota/status". Used by the web UI status endpoint.
 */
const char *app_ota_last_status(void);

/**
 * Marks the running image as valid (cancels the bootloader rollback-pending
 * state). Idempotent/logs once; call it from a healthy poll loop.
 */
void app_ota_confirm_running_image(void);
