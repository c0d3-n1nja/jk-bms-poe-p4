/* Runtime device configuration, persisted in the dedicated NVS partition
 * "cfg" so it survives firmware/OTA updates. See app_config.h for the
 * contract; the web UI (app_web) is the editor. */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "app_config.h"

static const char *TAG = "app_cfg";

#define CFG_PARTITION "cfg"
#define CFG_NAMESPACE "cfg"

/* Live configuration: static storage so the returned pointer (and the
 * strings inside it) stay valid for the whole runtime. Never mutated after
 * boot - changes are written to NVS only and applied by the reboot that
 * follows the save. */
static app_config_t s_cfg;

static const app_config_t s_defaults = {
    .web_password  = "",
    .use_dhcp      = true,
    .static_ip     = "",
    .static_netmask= "",
    .static_gateway= "",
    .static_dns    = "",
    .mqtt_uri      = "",
    .mqtt_user     = "",
    .mqtt_pass     = "",
    .mqtt_tls      = false,
    .rs485_debug   = false,
};

/* ------------------------------------------------------------------ */

bool app_config_valid_ipv4(const char *s)
{
    int a, b, c, d;
    char tail;

    if (s == NULL) {
        return false;
    }
    /* 4 groups, no leading +/- signs via tests below, no trailing tokens */
    if (sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    return (a >= 0 && a <= 255) && (b >= 0 && b <= 255) &&
           (c >= 0 && c <= 255) && (d >= 0 && d <= 255);
}

/* ------------------------------------------------------------------ */

static void read_str(nvs_handle_t h, const char *key, char *dst, size_t cap, const char *def)
{
    size_t len = cap;

    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        snprintf(dst, cap, "%s", def);               /* key missing: default */
        return;
    }
    dst[cap - 1] = '\0';                             /* defensive truncation */
    if (strlen(dst) > cap - 1) {
        ESP_LOGW(TAG, "Config value '%s' truncated to %u bytes", key, (unsigned)(cap - 1));
    }
}

static void read_bool(nvs_handle_t h, const char *key, bool *dst)
{
    uint8_t v;

    if (nvs_get_u8(h, key, &v) == ESP_OK) {
        *dst = (v != 0);
    }
}

void app_config_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_flash_init_partition(CFG_PARTITION);

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(CFG_PARTITION));
        err = nvs_flash_init_partition(CFG_PARTITION);
    }
    ESP_ERROR_CHECK(err);

    s_cfg = s_defaults;
    err = nvs_open_from_partition(CFG_PARTITION, CFG_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Config partition empty - defaults (DHCP, MQTT off, web UI open)");
        return;
    }

    read_str(h, "pw",     s_cfg.web_password,   sizeof(s_cfg.web_password),   "");
    read_bool(h, "dhcp",  &s_cfg.use_dhcp);
    read_str(h, "ip",     s_cfg.static_ip,      sizeof(s_cfg.static_ip),      "");
    read_str(h, "mask",   s_cfg.static_netmask, sizeof(s_cfg.static_netmask), "");
    read_str(h, "gw",     s_cfg.static_gateway, sizeof(s_cfg.static_gateway), "");
    read_str(h, "dns",    s_cfg.static_dns,     sizeof(s_cfg.static_dns),     "");
    read_str(h, "uri",    s_cfg.mqtt_uri,       sizeof(s_cfg.mqtt_uri),       "");
    read_str(h, "user",   s_cfg.mqtt_user,      sizeof(s_cfg.mqtt_user),      "");
    read_str(h, "pass",   s_cfg.mqtt_pass,      sizeof(s_cfg.mqtt_pass),      "");
    read_bool(h, "tls",   &s_cfg.mqtt_tls);
    read_bool(h, "dbg",   &s_cfg.rs485_debug);

    nvs_close(h);

    ESP_LOGI(TAG, "Config loaded: IP %s, MQTT %s%s, debug %s",
             s_cfg.use_dhcp ? "DHCP" : "static",
             s_cfg.mqtt_uri[0] != '\0' ? s_cfg.mqtt_uri : "(off)",
             s_cfg.mqtt_tls ? " +TLS" : "",
             s_cfg.rs485_debug ? "on" : "off");
    if (s_cfg.use_dhcp == false &&
        (!app_config_valid_ipv4(s_cfg.static_ip) ||
         !app_config_valid_ipv4(s_cfg.static_netmask) ||
         !app_config_valid_ipv4(s_cfg.static_gateway) ||
         !app_config_valid_ipv4(s_cfg.static_dns))) {
        ESP_LOGW(TAG, "Static IP settings incomplete -> falling back to DHCP");
        s_cfg.use_dhcp = true;                       /* self-heal against a bad save */
    }
}

const app_config_t *app_config_get(void)
{
    return &s_cfg;                                   /* static storage: stable pointers */
}

bool app_config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err;

    if (nvs_open_from_partition(CFG_PARTITION, CFG_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open config partition for writing");
        return false;
    }
    nvs_erase_all(h);

    err = nvs_set_str(h, "pw",   cfg->web_password);
    err |= nvs_set_u8(h, "dhcp", (uint8_t)(cfg->use_dhcp ? 1 : 0));
    err |= nvs_set_str(h, "ip",  cfg->static_ip);
    err |= nvs_set_str(h, "mask",cfg->static_netmask);
    err |= nvs_set_str(h, "gw",  cfg->static_gateway);
    err |= nvs_set_str(h, "dns", cfg->static_dns);
    err |= nvs_set_str(h, "uri", cfg->mqtt_uri);
    err |= nvs_set_str(h, "user",cfg->mqtt_user);
    err |= nvs_set_str(h, "pass",cfg->mqtt_pass);
    err |= nvs_set_u8(h, "tls",  (uint8_t)(cfg->mqtt_tls ? 1 : 0));
    err |= nvs_set_u8(h, "dbg",  (uint8_t)(cfg->rs485_debug ? 1 : 0));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config save failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Config saved (applies after reboot)");
    return true;
}

void app_config_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset: erasing the configuration partition '%s'", CFG_PARTITION);
    ESP_ERROR_CHECK(nvs_flash_erase_partition(CFG_PARTITION));
}
