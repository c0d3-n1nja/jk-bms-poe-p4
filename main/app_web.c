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
#include "app_log.h"

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
static char e_ourl[300], e_ourl_def[300], e_suv[80], e_serr[120];

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
    app_ota_version_info_t vi;
    char dev[16];
    uint8_t mac[6] = { 0 };
    char buf[1280];
    int n;

    if (!auth_ok(req)) {
        return ESP_FAIL;
    }
    cfg = app_config_get();
    app = esp_app_get_description();
    app_ota_get_version_info(&vi);

    json_escape(cfg->static_ip,      e_ip,   sizeof(e_ip));
    json_escape(cfg->static_netmask, e_mask, sizeof(e_mask));
    json_escape(cfg->static_gateway, e_gw,   sizeof(e_gw));
    json_escape(cfg->static_dns,     e_dns,  sizeof(e_dns));
    json_escape(cfg->mqtt_uri,       e_uri,  sizeof(e_uri));
    json_escape(cfg->mqtt_user,      e_user, sizeof(e_user));
    json_escape(cfg->ota_url,        e_ourl, sizeof(e_ourl));
    json_escape(CONFIG_JKBMS_OTA_DEFAULT_URL, e_ourl_def, sizeof(e_ourl_def));
    json_escape(vi.remote,           e_suv,  sizeof(e_suv));
    json_escape(vi.error,            e_serr, sizeof(e_serr));

    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(dev, sizeof(dev), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    n = snprintf(buf, sizeof(buf),
                 "{\"dhcp\":%s,"
                 "\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\","
                 "\"uri\":\"%s\",\"user\":\"%s\",\"tls\":%s,\"dbg\":%s,"
                 "\"pw_set\":%s,\"pass_set\":%s,"
                 "\"ota_url\":\"%s\",\"ota_default_url\":\"%s\",\"ota_status\":\"%s\","
                 "\"app_version\":\"%s\",\"partition\":\"%s\",\"device_id\":\"%s\","
                 "\"sw_update_checked\":%s,\"sw_update_available\":%s,"
                 "\"checking\":%s,\"sw_update_version\":\"%s\",\"sw_update_error\":\"%s\"}",
                 cfg->use_dhcp ? "true" : "false",
                 e_ip, e_mask, e_gw, e_dns,
                 e_uri, e_user,
                 cfg->mqtt_tls ? "true" : "false",
                 cfg->rs485_debug ? "true" : "false",
                 cfg->web_password[0] != '\0' ? "true" : "false",
                 cfg->mqtt_pass[0] != '\0' ? "true" : "false",
                 e_ourl,
                 e_ourl_def,
                 app_ota_last_status(),
                 app->version,
                 esp_ota_get_running_partition()->label,
                 dev,
                 vi.checked ? "true" : "false",
                 vi.checked && vi.available ? "true" : "false",
                 vi.checking ? "true" : "false",
                 e_suv,
                 e_serr);
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
    } else if (strcmp(key, "ota_url") == 0) {
        /* Text-overwrite semantics: empty = clear -> the build-time default
         * update URL is active again. Non-empty must be a firmware URL. */
        ok = copy_field(new_c->ota_url, sizeof(new_c->ota_url), val);
        if (ok && val[0] != '\0' &&
            strncmp(val, "http://", 7) != 0 && strncmp(val, "https://", 8) != 0) {
            *err = "update URL must start with http:// or https://";
            return false;
        }
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

    /* Start from the current values so empty password fields mean "keep".
     * Checkboxes are reset first: an unchecked box sends NO key at all, so
     * "absent" must mean false - otherwise unchecking (debug, TLS, switching
     * to static IP) could never revert a previously stored true. */
    new_c = *cur;
    new_c.use_dhcp = false;
    new_c.mqtt_tls = false;
    new_c.rs485_debug = false;
    if (!parse_form(body, &new_c, &err)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_FAIL;
    }
    if (new_c.use_dhcp == false &&
        (!app_config_valid_ipv4(new_c.static_ip) ||
         !app_config_valid_ipv4(new_c.static_netmask) ||
         !app_config_valid_ipv4(new_c.static_gateway) ||
         !app_config_valid_ipv4(new_c.static_dns))) {
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
    if (app_ota_start_configured()) {
        return httpd_resp_send(req, "Update gestartet (Status erscheint im 'Status'-Feld).", -1);
    }
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req,
                           "Update nicht moeglich: weder manuelle noch Standard-URL gesetzt oder "
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

static esp_err_t h_ota_check(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return ESP_FAIL;
    }
    if (app_ota_effective_url()[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Keine Update-URL konfiguriert (manuell + Standard leer).", -1);
    }
    (void)app_ota_trigger_version_check();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "Pruefung gestartet - 'Update-Status' aktualisiert sich.", -1);
}

/* GET /ota/version: snapshot of the update check for the web UI. */
static esp_err_t h_ota_version(httpd_req_t *req)
{
    app_ota_version_info_t vi;
    char buf[192];
    int n;

    if (!auth_ok(req)) {
        return ESP_FAIL;
    }
    app_ota_get_version_info(&vi);
    n = snprintf(buf, sizeof(buf),
                 "{\"installed\":\"%s\",\"remote\":\"%s\",\"available\":%s,"
                 "\"checked\":%s,\"checking\":%s,\"error\":\"%s\"}",
                 vi.installed, vi.remote,
                 vi.checked && vi.available ? "true" : "false",
                 vi.checked ? "true" : "false",
                 vi.checking ? "true" : "false",
                 vi.error);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON overflow");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

/* ------------------------------------------------------------------ */
/* Device log: /log = terminal page, /log/data = poll endpoint (JSON)   */
/* ------------------------------------------------------------------ */

/* Embedded terminal page (main/log_page.html) */
extern const unsigned char log_page_start[] asm("_binary_log_page_html_start");
extern const unsigned char log_page_end[]   asm("_binary_log_page_html_end");

#define LOG_POLL_MAX_LINES 64

static app_log_line_t s_logout[LOG_POLL_MAX_LINES];   /* poll result buffer */

static esp_err_t h_log(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)log_page_start,
                           (int)(log_page_end - log_page_start));
}

/** Poll endpoint of the terminal: GET /log/data?since=<seq>[&level=0..5][&clear=1].
 *  Answers immediately (the httpd worker task must not block - a streaming
 *  handler would stall /config.json & co. while the terminal is open).
 *  Also acts as the viewer heartbeat: activates the log redirect to the
 *  web ring while the terminal polls, deactivates itself after 10 s
 *  without a poll (back to the console). */
static esp_err_t h_log_data(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char q[120];
    char val[16];
    char esc[800];
    char line[900];
    uint32_t since = 0, last = 0;
    size_t n, w;
    bool clear = false, reset = false, ok;

    if (!auth_ok(req)) {
        return ESP_FAIL;
    }
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, NULL, 10);
        }
        if (httpd_query_key_value(q, "level", val, sizeof(val)) == ESP_OK) {
            (void)app_log_set_level(atoi(val));      /* dynamic log level */
        }
        if (httpd_query_key_value(q, "clear", val, sizeof(val)) == ESP_OK &&
            strcmp(val, "1") == 0) {
            clear = true;
        }
    }

    app_log_viewer_touch();                          /* heartbeat + redirect */
    if (clear) {
        app_log_clear();
    }
    n = app_log_lines_since(since, &last, &reset, s_logout,
                            sizeof(s_logout) / sizeof(s_logout[0]));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    w = (size_t)snprintf(line, sizeof(line),
                         "{\"last\":%lu,\"level\":%d,\"viewer\":%s,\"reset\":%s,"
                         "\"app\":\"v%s\",\"lines\":[",
                         (unsigned long)last,
                         (int)esp_log_get_default_level(),
                         app_log_viewer_active() ? "true" : "false",
                         reset ? "true" : "false",
                         app->version);
    ok = (w > 0 && w < sizeof(line)) &&
         httpd_resp_send_chunk(req, line, (ssize_t)w) == ESP_OK;

    for (size_t i = 0; ok && i < n; i++) {
        json_escape(s_logout[i].text, esc, sizeof(esc));
        w = (size_t)snprintf(line, sizeof(line), "%s\"%s\"", i ? "," : "", esc);
        ok = (w > 0 && w < sizeof(line)) &&
             httpd_resp_send_chunk(req, line, (ssize_t)w) == ESP_OK;
    }
    if (ok) {
        ok = httpd_resp_send_chunk(req, "]}", 2) == ESP_OK &&
             httpd_resp_send_chunk(req, NULL, 0) == ESP_OK;
    }
    if (!ok) {
        httpd_resp_send_chunk(req, NULL, 0);         /* terminate chunked resp */
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const httpd_uri_t k_uris[] = {
    { .uri = "/",            .method = HTTP_GET,  .handler = h_index        },
    { .uri = "/config.json", .method = HTTP_GET,  .handler = h_config_json  },
    { .uri = "/save",        .method = HTTP_POST, .handler = h_save         },
    { .uri = "/ota/install", .method = HTTP_POST, .handler = h_ota_install  },
    { .uri = "/ota/status",  .method = HTTP_GET,  .handler = h_ota_status   },
    { .uri = "/ota/check",   .method = HTTP_POST, .handler = h_ota_check    },
    { .uri = "/ota/version", .method = HTTP_GET,  .handler = h_ota_version  },
    { .uri = "/log",         .method = HTTP_GET,  .handler = h_log          },
    { .uri = "/log/data",    .method = HTTP_GET,  .handler = h_log_data     },
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
