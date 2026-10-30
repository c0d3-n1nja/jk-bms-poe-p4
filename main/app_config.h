#pragma once

#include <stdbool.h>
#include <stddef.h>

#define APP_CONFIG_WEB_PASSWORD_MAX 48
#define APP_CONFIG_IPSTR_MAX        16   /* "255.255.255.255" = 15 + NUL */
#define APP_CONFIG_MQTT_URI_MAX     128
#define APP_CONFIG_MQTT_USER_MAX    64
#define APP_CONFIG_MQTT_PASS_MAX    64
#define APP_CONFIG_OTA_URL_MAX      256  /* manual firmware update URL (app_ota.c uses the same cap) */

/**
 * Runtime device configuration, persisted in the dedicated NVS partition
 * "cfg" (survives firmware/OTA updates; see partitions.csv). Edited via the
 * built-in web UI (app_web).
 *
 * Defaults: DHCP on, MQTT off, no web password, RS485 debug off.
 * MQTT: empty URI = MQTT disabled entirely. mqtt_tls upgrades mqtt:// URIs
 * to mqtts:// at connect time (embedded CA certificate is used to verify
 * the server). static_dns is the main DNS in static mode and an optional
 * fallback DNS alongside the DHCP-leased one in DHCP mode.
 */
typedef struct {
    char   web_password[APP_CONFIG_WEB_PASSWORD_MAX]; /* empty = web UI unprotected */
    bool   use_dhcp;                                  /* false = static IPv4 below */
    char   static_ip[APP_CONFIG_IPSTR_MAX];
    char   static_netmask[APP_CONFIG_IPSTR_MAX];
    char   static_gateway[APP_CONFIG_IPSTR_MAX];
    char   static_dns[APP_CONFIG_IPSTR_MAX];
    char   mqtt_uri[APP_CONFIG_MQTT_URI_MAX];         /* "mqtt://host:port" / "mqtts://..." */
    char   mqtt_user[APP_CONFIG_MQTT_USER_MAX];
    char   mqtt_pass[APP_CONFIG_MQTT_PASS_MAX];
    bool   mqtt_tls;                                  /* +CA verify, mqtt<->mqtts upswitch */
    bool   rs485_debug;                               /* raw RS485 TX/RX hexdumps */
    char   ota_url[APP_CONFIG_OTA_URL_MAX];           /* manual update URL; empty = build-time default */
} app_config_t;

/** Loads the configuration (first boot / after reset: defaults). Call once at boot. */
void app_config_init(void);

/**
 * Current configuration. Returns a pointer into static storage; the strings
 * stay valid for the whole runtime (esp_mqtt_client keeps pointers to them).
 * The in-memory copy is never modified at runtime - changes apply on reboot.
 */
const app_config_t *app_config_get(void);

/**
 * Persists a complete configuration set to NVS (boot loader of truth for the
 * next boot; the caller reboots afterwards). Values must already be validated.
 * Returns false if NVS access fails.
 */
bool app_config_save(const app_config_t *cfg);

/** Erases the whole configuration partition (factory state). Caller reboots. */
void app_config_factory_reset(void);

/** Strict dotted-quad validator ("192.168.1.10"; rejects trailing tokens). */
bool app_config_valid_ipv4(const char *s);
