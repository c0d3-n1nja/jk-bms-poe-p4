/* Built-in web UI for runtime configuration (see app_web.h). Runs on
 * esp_http_server; state comes from app_config, persistence via
 * app_config_save followed by a reboot. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"
#include "mbedtls/base64.h"

#include "app_web.h"
#include "app_config.h"
#include "app_ota.h"

static const char *TAG = "app_web";

static httpd_handle_t s_srv = NULL;

/* Embedded configuration page (main/config_page.html) */
extern const unsigned char page_start[] asm("_binary_config_page_html_start");
extern const unsigned char page_end[]   asm("_binary_config_page_html_end");

/* ------------------------------------------------------------------ */
/* HTTP Basic auth (password-only; any username is accepted)            */
/* ------------------------------------------------------------------ */

static bool auth_ok(httpd_req_t *req)
{
    const app_config_t *cfg = app_config_get();
    char hdr[200];
    char dec[120];
    size_t olen = 0;
    const char *b64;
    char *colon;

    if (cfg->web_password[0] == '\0') {
        return true;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        goto deny;
    }
    /* Accept "Basic <b64>"; be lenient about the space after the scheme. */
    if (strncasecmp(hdr, "Basic", 5) != 0) {
        goto deny;
    }
    b64 = hdr + 5;
    while (*b64 == ' ' || *b64 == '\t') {
        b64++;
    }
    if (*b64 == '\0' ||
        mbedtls_base64_decode((unsigned char *)dec, sizeof(dec) - 1, &olen,
                              (const unsigned char *)b64, strlen(b64)) != 0) {
        goto deny;
    }
    dec[olen] = '\0';
    colon = strchr(dec, ':');
    if (colon == NULL || strcmp(colon + 1, cfg->web_password) != 0) {
        goto deny;
    }
    return true;
deny:
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"jkbms\"");
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Login required");
    return false;
}

/* ------------------------------------------------------------------ */
/* JSON helpers                                                         */
/* ------------------------------------------------------------------ */

/** Escapes a string for embedding into JSON (" \ and control chars). */
static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;

    for (; *src != '\0'; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) break;
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 7 >= cap) break;
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", (unsigned)c);
        } else {
            if (o + 1 >= cap) break;
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/* Scratch buffers for escaped strings (simple single-client usage). */
static char e_ip[64], e_mask[64], e_gw[64], e_dns[64], e_uri[300], e_user[160];

/* ------------------------------------------------------------------ */
/* Handlers                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t h_index(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)page_start,
                           (int)(page_end - page_start));
}

static esp_err_t h_config_json(httpd_req_t *req)
{
    const app_config_t *cfg;
    const esp_app_desc_t *app;
    char dev[16];
    uint8_t mac[6] = { 0 };
    char buf[1024];
    int n;

    if (!auth_ok(req)) {
        return ESP_FAIL;
    }
    cfg = app_config_get();
    app = esp_app_get_description();

    json_escape(cfg->static_ip,      e_ip,   sizeof(e_ip));
    json_escape(cfg->static_netmask, e_mask, sizeof(e_mask));
    json_escape(cfg->static_gateway, e_gw,   sizeof(e_gw));
    json_escape(cfg->static_dns,     e_dns,  sizeof(e_dns));
    json_escape(cfg->mqtt_uri,       e_uri,  sizeof(e_uri));
    json_escape(cfg->mqtt_user,      e_user, sizeof(e_user));

    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(dev, sizeof(dev), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    n = snprintf(buf, sizeof(buf),
                 "{\"dhcp\":%s,"
                 "\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\","
                 "\"uri\":\"%s\",\"user\":\"%s\",\"tls\":%s,\"dbg\":%s,"
                 "\"pw_set\":%s,\"pass_set\":%s,"
                 "\"ota_url\":\"%s\",\"ota_status\":\"%s\","
                 "\"app_version\":\"%s\",\"partition\":\"%s\",\"device_id\":\"%s\"}",
                 cfg->use_dhcp ? "true" : "false",
                 e_ip, e_mask, e_gw, e_dns,
                 e_uri, e_user,
                 cfg->mqtt_tls ? "true" : "false",
                 cfg->rs485_debug ? "true" : "false",
                 cfg->web_password[0] != '\0' ? "true" : "false",
                 cfg->mqtt_pass[0] != '\0' ? "true" : "false",
                 CONFIG_JKBMS_OTA_DEFAULT_URL,
                 app_ota_last_status(),
                 app->version,
                 esp_ota_get_running_partition()->label,
                 dev);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON overflow");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

/* ------------------------------------------------------------------ */
/* Form parsing (application/x-www-form-urlencoded)                     */
/* ------------------------------------------------------------------ */

static void url_decode(char *s)
{
    char *o = s;

    while (*s != '\0') {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], '\0' };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

/** Copies val into dst; false if it does not fit. */
static bool copy_field(char *dst, size_t cap, const char *val)
{
    if (strlen(val) + 1 > cap) {
        return false;
    }
    strcpy(dst, val);
    return true;
}

/**
 * Applies one form key to the new config. Form semantics:
 *  - "pw"/"pass" non-empty      : set password (else keep the current one)
 *  - "pw_clear"/"pass_clear"=on : clear the password (text field stays empty)
 *  - all other text fields      : overwrite (empty value clears the field)
 *  - checkbox key               : "1"/"on"/"true" = true, absent = false
 */
static bool apply_key(app_config_t *new_c, const char *key, char *val, const char **err)
{
    bool flag = (val[0] == '1' || strcmp(val, "on") == 0 || strcasecmp(val, "true") == 0);
    bool ok = true;

    if (strcmp(key, "pw") == 0) {
        if (val[0] != '\0') {
            ok = copy_field(new_c->web_password, sizeof(new_c->web_password), val);
        }
    } else if (strcmp(key, "pw_clear") == 0) {
        if (flag) {
            new_c->web_password[0] = '\0';
        }
    } else if (strcmp(key, "dhcp") == 0) {
        new_c->use_dhcp = flag;
    } else if (strcmp(key, "ip") == 0) {
        ok = copy_field(new_c->static_ip, sizeof(new_c->static_ip), val);
    } else if (strcmp(key, "mask") == 0) {
        ok = copy_field(new_c->static_netmask, sizeof(new_c->static_netmask), val);
    } else if (strcmp(key, "gw") == 0) {
        ok = copy_field(new_c->static_gateway, sizeof(new_c->static_gateway), val);
    } else if (strcmp(key, "dns") == 0) {
        ok = copy_field(new_c->static_dns, sizeof(new_c->static_dns), val);
    } else if (strcmp(key, "uri") == 0) {
        ok = copy_field(new_c->mqtt_uri, sizeof(new_c->mqtt_uri), val);
    } else if (strcmp(key, "user") == 0) {
        ok = copy_field(new_c->mqtt_user, sizeof(new_c->mqtt_user), val);
    } else if (strcmp(key, "pass") == 0) {
        if (val[0] != '\0') {
            ok = copy_field(new_c->mqtt_pass, sizeof(new_c->mqtt_pass), val);
        }
    } else if (strcmp(key, "pass_clear") == 0) {
        if (flag) {
            new_c->mqtt_pass[0] = '\0';
        }
    } else if (strcmp(key, "tls") == 0) {
        new_c->mqtt_tls = flag;
    } else if (strcmp(key, "dbg") == 0) {
        new_c->rs485_debug = flag;
    } else {
        return true;                                 /* unknown keys ignored */
    }

    if (!ok) {
        *err = "value too long for field";
    }
    return ok;
}

static bool parse_form(char *body, app_config_t *new_c, const char **err)
{
    char *pair, *save = NULL;

    for (pair = strtok_r(body, "&", &save); pair != NULL; pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (eq == NULL) {
            continue;                                /* flag without '=': ignore */
        }
        *eq = '\0';
        url_decode(pair);
        url_decode(eq + 1);
        if (!apply_key(new_c, pair, eq + 1, err)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* POST /save                                                           */
/* ------------------------------------------------------------------ */

static esp_err_t h_save(httpd_req_t *req)
{
    const app_config_t *cur = app_config_get();
    app_config_t new_c;
    char *body;
    int len = (int)req->content_len;
    const char *err = "invalid form";

    if (!auth_ok(req)) {
        return ESP_FAIL;
    }
    if (len <= 0 || len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body length");
        return ESP_FAIL;
    }
    body = malloc((size_t)len + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    if (httpd_req_recv(req, body, (size_t)len) != len) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    /* Start from the current values so empty password fields mean "keep". */
    new_c = *cur;
    if (!parse_form(body, &new_c, &err) ||
        (new_c.use_dhcp == false &&
         (!app_config_valid_ipv4(new_c.static_ip) ||
          !app_config_valid_ipv4(new_c.static_netmask) ||
          !app_config_valid_ipv4(new_c.static_gateway) ||
          !app_config_valid_ipv4(new_c.static_dns)))) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid values (static mode needs all four IPv4 addresses)");
        return ESP_FAIL;
    }
    free(body);

    /* MQTT: empty URI = off; otherwise the scheme must be mqtt(s):// and
     * short enough that the TLS upswitch keeps two reserve bytes. */
    if (new_c.mqtt_uri[0] != '\0') {
        bool is_mqtt  = strncmp(new_c.mqtt_uri, "mqtt://", 7) == 0;
        bool is_mqtts = strncmp(new_c.mqtt_uri, "mqtts://", 8) == 0;
        if ((!is_mqtt && !is_mqtts) ||
            strlen(new_c.mqtt_uri) >= sizeof(new_c.mqtt_uri) - 2) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "invalid MQTT URI (mqtt://host:port)");
            return ESP_FAIL;
        }
    }

    if (!app_config_save(&new_c)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config saved via web UI - rebooting in 1 s");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    (void)httpd_resp_send(req, "Konfiguration gespeichert. Geraet startet neu...", -1);
    vTaskDelay(pdMS_TO_TICKS(1000));                 /* let the response drain */
    esp_restart();                                   /* config applies on reboot */
    return ESP_OK;                                   /* not reached */
}

static esp_err_t h_ota_install(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (app_ota_start_default()) {
        return httpd_resp_send(req, "Update gestartet (Status erscheint im 'Status'-Feld).", -1);
    }
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req,
                           "Update nicht moeglich: keine Standard-URL (Build-Zeit) gesetzt oder "
                           "bereits ein Update aktiv.", -1);
}

static esp_err_t h_ota_status(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, app_ota_last_status(), -1);
}

static const httpd_uri_t k_uris[] = {
    { .uri = "/",            .method = HTTP_GET,  .handler = h_index        },
    { .uri = "/config.json", .method = HTTP_GET,  .handler = h_config_json  },
    { .uri = "/save",        .method = HTTP_POST, .handler = h_save         },
    { .uri = "/ota/install", .method = HTTP_POST, .handler = h_ota_install  },
    { .uri = "/ota/status",  .method = HTTP_GET,  .handler = h_ota_status   },
};

/* ------------------------------------------------------------------ */

esp_err_t app_web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    size_t i;
    esp_err_t err;

    if (s_srv != NULL) {
        return ESP_OK;                               /* already running */
    }
    cfg.stack_size = 8192;                           /* JSON build + form parse */
    err = httpd_start(&s_srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_srv = NULL;
        return err;
    }
    for (i = 0; i < sizeof(k_uris) / sizeof(k_uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_srv, &k_uris[i]));
    }
    ESP_LOGI(TAG, "Web UI up on port 80 (%s)",
             app_config_get()->web_password[0] != '\0' ? "password protected" : "no password");
    return ESP_OK;
}

void app_web_stop(void)
{
    if (s_srv != NULL) {
        httpd_stop(s_srv);
        s_srv = NULL;
        ESP_LOGI(TAG, "Web UI stopped");
    }
}
