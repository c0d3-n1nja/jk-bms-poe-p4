#pragma once

#include <stdbool.h>
#include <stddef.h>

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

/** Result of the online update check (compare installed vs. remote). */
typedef struct {
    bool checked;            /* at least one completed version check */
    bool available;          /* remote version is newer than the running one */
    bool checking;           /* a check is currently running */
    char installed[33];      /* version of the running image (app desc, max 32) */
    char remote[33];         /* fetched remote version, "" if never fetched */
} app_ota_version_info_t;

/* ------------------------------------------------------------------ */
/* OTA download                                                          */
/* ------------------------------------------------------------------ */

/**
 * Starts the OTA download in an own task. Accepts only http(s) URLs and
 * rejects a second request while an update is already in progress.
 * Returns false on invalid URL or task-creation failure.
 */
bool app_ota_start_from_url(const char *url);

/**
 * Starts the OTA download from the ACTIVE update URL: the manually
 * configured URL of the web UI (runtime config 'cfg'); if that is empty
 * the build-time default (CONFIG_JKBMS_OTA_DEFAULT_URL) is used. Returns
 * false if neither is set or the request is rejected (busy).
 */
bool app_ota_start_configured(void);

/**
 * Effective update URL for button/display purposes: manual web-UI URL or
 * the build-time default. Never NULL; may be "" when nothing is set.
 */
const char *app_ota_effective_url(void);

/**
 * Last reachable OTA status text ("idle", "downloading 30%", "success,
 * rebooting", "failed: ...") - the same string that was last published to
 * "<prefix>/ota/status". Used by the web UI status endpoint.
 */
const char *app_ota_last_status(void);

/* ------------------------------------------------------------------ */
/* Online update check (plain "version" file, no download)               */
/* ------------------------------------------------------------------ */

/**
 * Requests an immediate version check and starts the background checker
 * task on first use (it then also performs the periodic automatic check,
 * CONFIG_JKBMS_VERSION_CHECK_INTERVAL_S; 0 = automatic checks disabled).
 *
 * The check fetches a plain-text file ("version" next to the active update
 * URL, or CONFIG_JKBMS_VERSION_URL) and compares the version there with
 * the running one. The result can be read via app_ota_get_version_info(),
 * is included in the retained MQTT device topic ("<prefix>/device") and
 * is shown in the web UI. Returns false only if the checker task cannot
 * be created.
 */
bool app_ota_trigger_version_check(void);

/** Snapshot of the current version-check state (values are copies). */
void app_ota_get_version_info(app_ota_version_info_t *out);

/**
 * Marks the running image as valid (cancels the bootloader rollback-pending
 * state). Idempotent/logs once; call it from a healthy poll loop.
 */
void app_ota_confirm_running_image(void);
