/* OTA firmware update via MQTT-triggered URL download (esp_https_ota).
 * See app_ota.h for the usage contract. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "sdkconfig.h"

#include "app_ota.h"
#include "app_mqtt.h"
#include "app_config.h"

static const char *TAG = "app_ota";

#define OTA_URL_MAX      256
#define OTA_TASK_STACK   12288   /* TLS buffer + ota ops need headroom */
#define OTA_PROGRESS_STEP 10     /* status update every 10 % */

/* --- version check -------------------------------------------------- */
#define VER_FILE_MAX     OTA_URL_MAX
#define VER_READ_MAX     48       /* version string cap (few bytes expected) */
#define VER_TASK_STACK   8192
#define VER_POLL_MS      1000     /* scheduler tick: reads flags only      */
#define VER_RETRY_US     ((int64_t)60 * 1000000)              /* failed check, before first success */
#define VER_RETRY_SLOW_US ((int64_t)10 * 60 * 1000000)        /* failed check, after a success */
#define VER_FALLBACK_US  ((int64_t)1000 * 1000 * 1000 * 60 * 60 * 24 * 365) /* no auto check */

/* Reuse the CA certificate embedded for MQTTs (server verification). */
extern const unsigned char server_ca_pem_start[] asm("_binary_ca_cert_pem_start");

static char s_url[OTA_URL_MAX];
static bool s_busy = false;
static char s_status[64] = "idle";

/* Version check state (single writer: the "ver_chk" task). */
static TaskHandle_t s_vchk_task = NULL;
static portMUX_TYPE s_vchk_mux = portMUX_INITIALIZER_UNLOCKED;   /* protects s_remote + flags */
static volatile bool s_vchk_requested = false;   /* next loop pass does a fetch */
static volatile bool s_vchk_busy = false;        /* fetch currently running */
static volatile bool s_vchk_checked = false;     /* at least one fetch finished */
static volatile bool s_vchk_available = false;   /* remote > installed         */
static char s_installed[33] = "";                /* version of the running app */
static char s_remote[VER_READ_MAX] = "";
static char s_vchk_error[48] = "";               /* last failure reason for web/MQTT */
static int64_t s_vchk_next_us = 0;               /* due time of the auto check */

/* Every visible OTA state goes through here: keeps the last status for the
 * web UI (app_ota_last_status) and publishes it to MQTT (retained). */
static void set_status(const char *status)
{
    snprintf(s_status, sizeof(s_status), "%s", status);
    app_mqtt_publish_ota_status(s_status);
}

/* ------------------------------------------------------------------ */

static void ota_task(void *arg)
{
    esp_http_client_config_t http = {
        .url = s_url,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
        .buffer_size = 8192,
        .buffer_size_tx = 2048,
    };
    esp_https_ota_config_t ota = { .http_config = &http };
    esp_https_ota_handle_t handle = NULL;
    const esp_partition_t *running;
    char status[64];
    int total, last_pct = 0;
    esp_err_t err;

    running = esp_ota_get_running_partition();

#if CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    /* TEMPORARY insecure mode (CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y):
     * no CA is attached, esp-tls runs with MBEDTLS_SSL_VERIFY_NONE. The
     * embedded ca_cert.pem is still a placeholder, so attach it only once
     * it holds the real CA chain. */
    ESP_LOGW(TAG, "OTA https: TLS server verification DISABLED (temporary)");
#else
    if (strncmp(s_url, "https://", 8) == 0) {
        /* Server verification: accept only certs rooted in the embedded CA. */
        http.cert_pem = (const char *)server_ca_pem_start;
    }
#endif

    set_status("download started");
    ESP_LOGI(TAG, "OTA download started: %s (running from %s)", s_url, running->label);

    err = esp_https_ota_begin(&ota, &handle);        /* v5 API: size is discovered during download */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        snprintf(status, sizeof(status), "failed: %s", esp_err_to_name(err));
        set_status(status);
        goto exit_fail;
    }

    total = esp_https_ota_get_image_size(handle);    /* -1 on chunked encoding */
    ESP_LOGI(TAG, "Image size %d bytes", total);

    while (true) {
        int got, pct;

        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;                                   /* done or hard error */
        }
        got = esp_https_ota_get_image_len_read(handle);
        if (total > 0) {
            pct = (int)(((int64_t)got * 100) / total);
            if (pct / OTA_PROGRESS_STEP > last_pct / OTA_PROGRESS_STEP) {
                snprintf(status, sizeof(status), "downloading %d%%", pct);
                set_status(status);
            }
            last_pct = pct;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        snprintf(status, sizeof(status), "failed: %s", esp_err_to_name(err));
        set_status(status);
        (void)esp_https_ota_abort(handle);
        goto exit_fail;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        snprintf(status, sizeof(status), "failed: %s", esp_err_to_name(err));
        set_status(status);
        goto exit_fail;
    }

    /* The new image boots in "pending verify" state (rollback guard);
     * confirm_app_running_image() in the new firmware completes it. */
    set_status("success, rebooting");
    ESP_LOGI(TAG, "OTA installed - rebooting");

    vTaskDelay(pdMS_TO_TICKS(500));                  /* let MQTT flush */
    esp_restart();                                   /* noreturn */

exit_fail:
    s_busy = false;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */

bool app_ota_start_from_url(const char *url)
{
    size_t len;

    if (url == NULL) {
        return false;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        set_status("error: URL must be http(s)://...");
        ESP_LOGW(TAG, "OTA request rejected: invalid URL scheme");
        return false;
    }
    if (s_busy) {
        set_status("busy: update already in progress");
        return true;
    }
    len = strlen(url);
    if (len == 0u || len >= sizeof(s_url)) {
        set_status("error: URL too long");
        return false;
    }

    memcpy(s_url, url, len + 1);
    s_busy = true;
    if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK, NULL, 5, NULL) != pdPASS) {
        s_busy = false;
        set_status("error: cannot start OTA task");
        ESP_LOGE(TAG, "xTaskCreate(ota) failed");
        return false;
    }
    return true;
}

void app_ota_confirm_running_image(void)
{
    static bool done = false;
    const esp_partition_t *running;
    const esp_app_desc_t *desc;
    esp_ota_img_states_t state;

    if (done) {
        return;
    }

    running = esp_ota_get_running_partition();
    desc = esp_app_get_description();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        ESP_LOGI(TAG, "App %s v%s running from %s (state %d)",
                 desc->project_name, desc->version, running->label, (int)state);
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            /* Boot proven healthy: MQTT up + first successful BMS data */
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Rollback pending cancelled - image marked valid");
        }
    } else {
        ESP_LOGI(TAG, "App %s v%s running from %s", desc->project_name,
                 desc->version, running->label);
    }
    done = true;
}

/* ------------------------------------------------------------------ */
/* Online update check: plain "version" file, compare, no download      */
/* ------------------------------------------------------------------ */

const char *app_ota_effective_url(void)
{
    const app_config_t *cfg = app_config_get();

    return cfg->ota_url[0] != '\0' ? cfg->ota_url : CONFIG_JKBMS_OTA_DEFAULT_URL;
}

bool app_ota_start_configured(void)
{
    const char *url = app_ota_effective_url();

    if (url[0] == '\0') {
        ESP_LOGW(TAG, "No update URL configured (web UI + CONFIG_JKBMS_OTA_DEFAULT_URL empty)");
        set_status("error: no update URL configured");
        return false;
    }
    return app_ota_start_from_url(url);
}

const char *app_ota_last_status(void)
{
    return s_status;
}

/** Reads the version of the running image (app descriptor is only partially NUL-terminated by the build). */
static void get_installed_version(char dst[33])
{
    const esp_app_desc_t *desc = esp_app_get_description();

    memcpy(dst, desc->version, 32);
    dst[32] = '\0';
}

/** Accepts only "v<d.d.d-test-free>" shapes: digits and dots, optional leading 'v'. */
static bool plausible_version(const char *s)
{
    int digits = 0;

    if (s == NULL || *s == '\0') {
        return false;
    }
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    for (; *s != '\0'; s++) {
        if (*s >= '0' && *s <= '9') {
            digits++;
        } else if (*s != '.') {
            return false;                            /* anything else: untrusted */
        }
    }
    return digits > 0;
}

/**
 * Numeric dotted compare ("v0.0.4" vs "v0.0.3"); missing groups and
 * suffixes like "0.0.4-rc1" behave as 0 for their group and any remaining
 * digits decide (so "0.0.3" < "0.0.3.1"). Non-layered garbage stays "not
 * newer" - plausible_version() already filters most of it.
 */
static bool version_is_newer(const char *remote, const char *installed)
{
    int guard;

    for (guard = 0; guard < 16; guard++) {
        if (*remote == 'v' || *remote == 'V') remote++;
        if (*installed == 'v' || *installed == 'V') installed++;
        long r = strtol(remote, (char **)&remote, 10);
        long l = strtol(installed, (char **)&installed, 10);
        if (r != l) {
            return r > l;
        }
        if (*remote == '.') remote++;
        if (*installed == '.') installed++;
        if ((*remote == '\0' || *installed == '\0') == false) {
            continue;
        }
        return *remote != '\0';                      /* "1.2.3.4" > "1.2.3", else equal */
    }
    return false;                                    /* max 16 groups reached */
}

/**
 * Version-file URL: CONFIG_JKBMS_VERSION_URL if set, else the active
 * update URL (manual, else default) with the last path element replaced
 * by "version" - e.g. ".../releases/latest/download/jkbms_mqtt_poe_p4.bin"
 * becomes ".../releases/latest/download/version". False if nothing usable.
 */
static bool build_version_url(char *dst, size_t cap)
{
    const char *src = app_ota_effective_url();
    const char *slash;

    if (CONFIG_JKBMS_VERSION_URL[0] != '\0') {
        snprintf(dst, cap, "%s", CONFIG_JKBMS_VERSION_URL);
        return true;
    }
    if (src[0] == '\0') {
        return false;
    }
    slash = strrchr(src, '/');
    if (slash == NULL || slash == src ||
        (slash - 1 >= src && slash[-1] == '/')) {    /* host root "http://host" without path */
        return false;
    }
    snprintf(dst, cap, "%.*sversion", (int)(slash - src), src);
    return dst[0] != '\0' && strlen(dst) < cap;
}

/**
 * Collects HTTP_EVENT_ON_DATA body chunks (final status 200 only - the
 * redirect bodies of GitHub are ignored) into a tiny buffer. This pattern
 * with esp_http_client_perform() is REQUIRED here: perform() is the only
 * API that follows redirects (GitHub serves the version file via a 302 to
 * objects.githubusercontent.com); open()/fetch_headers()/read() would stop
 * at the 302 with an empty body.
 */
typedef struct {
    char buf[VER_READ_MAX];
    int  len;
    bool truncated;
} ver_rx_t;

static esp_err_t ver_http_event(esp_http_client_event_t *evt)
{
    ver_rx_t *rx = (ver_rx_t *)evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA || rx == NULL ||
        evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (esp_http_client_get_status_code(evt->client) != 200) {
        return ESP_OK;                                  /* 302/4xx body: ignore */
    }
    int space = (int)sizeof(rx->buf) - 1 - rx->len;
    if (space <= 0) {
        rx->truncated = true;
        return ESP_OK;
    }
    int copy = evt->data_len < space ? evt->data_len : space;
    memcpy(rx->buf + rx->len, evt->data, copy);
    rx->len += copy;
    if (copy < evt->data_len) {
        rx->truncated = true;
    }
    return ESP_OK;
}

/** Stores the last failure reason (short, web/MQTT display friendly). */
static void set_vchk_error(const char *fmt, ...)
{
    char tmp[48];
    va_list args;

    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    tmp[sizeof(tmp) - 1] = '\0';
    taskENTER_CRITICAL(&s_vchk_mux);
    memcpy(s_vchk_error, tmp, sizeof(tmp));
    taskEXIT_CRITICAL(&s_vchk_mux);
}

/** One synchronous fetch of the version file + compare + state update. */
static void version_attempt(void)
{
    ver_rx_t rx = { .len = 0, .truncated = false };
    esp_http_client_config_t http = {
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .event_handler = ver_http_event,
        .user_data = &rx,
    };
    esp_http_client_handle_t client;
    const char *inst;      /* points into s_installed during the attempt */
    char url[VER_FILE_MAX];
    int status = 0;
    bool ok = false;

    s_vchk_busy = true;
    get_installed_version(s_installed);
    inst = s_installed;

    if (!build_version_url(url, sizeof(url))) {
        /* Nothing configured/derivable: warn once, then wait for a manual
         * trigger (web UI or reboot) instead of retrying in a tight loop. */
        ESP_LOGW(TAG, "Version check skipped: no update URL configured (web UI + CONFIG_JKBMS_OTA_DEFAULT_URL)");
        set_vchk_error("no update URL configured");
        s_vchk_next_us = esp_timer_get_time() + VER_FALLBACK_US;
        s_vchk_busy = false;
        return;
    }
    http.url = url;

#if CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    /* TEMPORARY insecure mode: TLS still encrypts, but the server is not
     * validated (ca_cert.pem is still a placeholder). */
    ESP_LOGW(TAG, "Version check https: TLS server verification DISABLED (temporary)");
#else
    if (strncmp(url, "https://", 8) == 0) {
        http.cert_pem = (const char *)server_ca_pem_start;
    }
#endif

    client = esp_http_client_init(&http);
    if (client == NULL) {
        /* URL parse/config problem (DNS errors happen later, at open):
         * not retryable automatically -> manual trigger only. */
        ESP_LOGE(TAG, "Version check: esp_http_client_init failed for %s", url);
        set_vchk_error("invalid URL");
        s_vchk_next_us = esp_timer_get_time() + VER_FALLBACK_US;
        s_vchk_busy = false;
        return;
    }

    esp_err_t err = esp_http_client_perform(client);    /* follows redirects */
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        set_vchk_error("request failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "Version check: request to %s failed: %s", url, esp_err_to_name(err));
    } else if (status != 200) {
        set_vchk_error("HTTP %d", status);
        ESP_LOGW(TAG, "Version check: HTTP %d from %s (version file present in the release?)", status, url);
    } else if (rx.truncated) {
        set_vchk_error("response too long");
        ESP_LOGW(TAG, "Version check: response longer than %u bytes", (unsigned)(VER_READ_MAX - 1));
    } else if (rx.len == 0) {
        set_vchk_error("empty response");
        ESP_LOGW(TAG, "Version check: empty response body");
    } else {
        /* Trim whitespace/CR/LF around the token, then trust-check it. */
        char *p = rx.buf, *e = rx.buf + rx.len;
        while (p < e && *p <= ' ') {
            p++;
        }
        while (e > p && e[-1] <= ' ') {
            e--;
        }
        *e = '\0';
        if (plausible_version(p)) {
            char candidate[VER_READ_MAX];
            bool newer;

            snprintf(candidate, sizeof(candidate), "%s", p);
            newer = version_is_newer(candidate, inst);
            taskENTER_CRITICAL(&s_vchk_mux);
            memcpy(s_remote, candidate, sizeof(candidate));
            s_vchk_available = newer;
            s_vchk_checked = true;
            taskEXIT_CRITICAL(&s_vchk_mux);
            ok = true;
            set_vchk_error("");
            if (newer) {
                ESP_LOGW(TAG, "Update available: remote %s is newer than the running %s", candidate, inst);
            } else {
                ESP_LOGI(TAG, "Version check: %s - running %s is up to date", candidate, inst);
            }
        } else {
            set_vchk_error("invalid version data");
            ESP_LOGW(TAG, "Version check: '%s' does not look like a version string", rx.buf);
        }
    }

    /* Result / failure -> schedule the next automatic attempt. Failures retry
     * quickly UNTIL the first success (covers "network needs a moment" after
     * boot); afterwards a broken server is only re-tried at the normal
     * interval (or never, if the automatic check is disabled: manual only). */
    s_vchk_next_us = esp_timer_get_time();
    if (ok && CONFIG_JKBMS_VERSION_CHECK_INTERVAL_S > 0) {
        s_vchk_next_us += (int64_t)CONFIG_JKBMS_VERSION_CHECK_INTERVAL_S * 1000000;
    } else if (!ok && CONFIG_JKBMS_VERSION_CHECK_INTERVAL_S > 0 && !s_vchk_checked) {
        s_vchk_next_us += VER_RETRY_US;
    } else if (!ok && CONFIG_JKBMS_VERSION_CHECK_INTERVAL_S > 0) {
        s_vchk_next_us += VER_RETRY_SLOW_US;
    } else {
        s_vchk_next_us += VER_FALLBACK_US;           /* manual check only */
    }
    s_vchk_busy = false;
    if (s_vchk_checked) {
        /* Update the retained device topic with the new update state. */
        app_mqtt_publish_device_info();
    }
}

/** Persistent scheduler: fetches on request flags and keeps the cadence. */
static void version_check_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_vchk_requested || esp_timer_get_time() >= s_vchk_next_us) {
            s_vchk_requested = false;
            version_attempt();
        }
        vTaskDelay(pdMS_TO_TICKS(VER_POLL_MS));
    }
}

bool app_ota_trigger_version_check(void)
{
    if (s_vchk_task == NULL) {
        if (xTaskCreate(version_check_task, "ver_chk", VER_TASK_STACK, NULL, 4, &s_vchk_task) != pdPASS) {
            s_vchk_task = NULL;
            ESP_LOGE(TAG, "xTaskCreate(ver_chk) failed");
            return false;
        }
        ESP_LOGI(TAG, "Update check started (%d s interval, URL %s)",
                 CONFIG_JKBMS_VERSION_CHECK_INTERVAL_S,
                 CONFIG_JKBMS_VERSION_URL[0] != '\0' ? CONFIG_JKBMS_VERSION_URL
                                                     : "(derived from the update URL)");
    }
    s_vchk_requested = true;                         /* immediate attempt */
    return true;
}

void app_ota_get_version_info(app_ota_version_info_t *out)
{
    if (out == NULL) {
        return;
    }
    get_installed_version(out->installed);
    taskENTER_CRITICAL(&s_vchk_mux);
    memcpy(out->remote, s_remote, sizeof(out->remote));
    memcpy(out->error, s_vchk_error, sizeof(out->error));
    out->checked   = s_vchk_checked;
    out->available = s_vchk_available;
    out->checking  = s_vchk_busy;
    taskEXIT_CRITICAL(&s_vchk_mux);
}
