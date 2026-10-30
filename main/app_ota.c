/* OTA firmware update via MQTT-triggered URL download (esp_https_ota).
 * See app_ota.h for the usage contract. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "sdkconfig.h"

#include "app_ota.h"
#include "app_mqtt.h"

static const char *TAG = "app_ota";

#define OTA_URL_MAX      256
#define OTA_TASK_STACK   12288   /* TLS buffer + ota ops need headroom */
#define OTA_PROGRESS_STEP 10     /* status update every 10 % */

/* Reuse the CA certificate embedded for MQTTs (server verification). */
extern const unsigned char server_ca_pem_start[] asm("_binary_ca_cert_pem_start");

static char s_url[OTA_URL_MAX];
static bool s_busy = false;
static char s_status[64] = "idle";

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

bool app_ota_start_default(void)
{
    const char *url = CONFIG_JKBMS_OTA_DEFAULT_URL;

    if (url[0] == '\0') {
        ESP_LOGW(TAG, "No default OTA URL configured (CONFIG_JKBMS_OTA_DEFAULT_URL)");
        return false;
    }
    return app_ota_start_from_url(url);
}

const char *app_ota_last_status(void)
{
    return s_status;
}
