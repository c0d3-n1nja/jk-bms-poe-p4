#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "sdkconfig.h"
#include "jkbms.h"
#include "app_mqtt.h"
#include "app_ota.h"
#include "app_config.h"

static const char *TAG = "app_mqtt";

/* Embedded CA cert (main/ca_cert.pem), only used with mqtts:// */
extern const unsigned char server_ca_pem_start[] asm("_binary_ca_cert_pem_start");
extern const unsigned char server_ca_pem_end[]   asm("_binary_ca_cert_pem_end");

#define STATE_JSON_BUF   3072
#define DISCOVERY_BUF    768

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static bool s_started = false;

/* Effective broker URI after the optional mqtt<->mqtts scheme switch.
 * Static storage: esp_mqtt_client keeps the POINTER (does not copy the
 * string), so it must outlive the client. The config struct itself lives
 * in app_config.c's static area, the same way. */
static char s_uri_eff[APP_CONFIG_MQTT_URI_MAX + 2];

static char s_dev_id[16];               /* e.g. "ABCDEF123456" */
static char s_client_id[48];
static char s_state_topic[96];
static char s_mqtt_available_topic[96];
static char s_bms_available_topic[96];
static char s_ota_topic[96];
static char s_ota_status_topic[96];

/* Last known BMS state; initially offline until a valid poll succeeds. */
static volatile bool s_bms_available = false;
static char s_json[STATE_JSON_BUF];
static char s_last_json[STATE_JSON_BUF];  /* last successfully published state */
static int64_t s_last_publish_us = 0;

/* ------------------------------------------------------------------ */

static bool publish(const char *topic, const char *data, int qos, bool retain)
{
    if (s_client == NULL || !s_connected) {
        return false;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, data, 0, qos, retain);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Publish to '%s' failed (client busy/offline)", topic);
        return false;
    }
    return true;
}

void app_mqtt_publish_bms_available(bool available)
{
    s_bms_available = available;

    publish(s_bms_available_topic,
            available ? "online" : "offline",
            1, true);
}

/** OTA progress/result (retained; see app_ota.h). */
void app_mqtt_publish_ota_status(const char *status)
{
    publish(s_ota_status_topic, status, 1, true);
}
/* ------------------------------------------------------------------ */
/* JSON state message                                                   */
/* ------------------------------------------------------------------ */

static void build_json(const jkbms_data_t *o, bool link_up, char *dst, size_t cap)
{
    size_t n = 0;
    int i;

#define APPEND(...)                                                            \
    do {                                                                       \
        int _w = snprintf(dst + n, cap - n, __VA_ARGS__);                      \
        if (_w < 0) return;                                                    \
        if ((size_t)_w >= cap - n) { n = cap - 1; dst[cap - 1] = '\0'; return; }\
        n += (size_t)_w;                                                       \
    } while (0)

    APPEND("{");
    APPEND("\"link_up\":%s,", link_up ? "true" : "false");
    if (!link_up) {
        APPEND("}");
        return;
    }

    APPEND("\"cell_count\":%d,", o->cell_count);
    for (i = 0; i < o->cell_count; i++) {
        APPEND("\"cell_voltage_%d\":%.3f,", i + 1, o->cell_voltage[i]);
    }
    APPEND("\"min_cell_voltage\":%.3f,", o->min_cell_voltage);
    APPEND("\"min_cell_voltage_cell\":%d,", o->min_voltage_cell);
    APPEND("\"max_cell_voltage\":%.3f,", o->max_cell_voltage);
    APPEND("\"max_cell_voltage_cell\":%d,", o->max_voltage_cell);
    APPEND("\"delta_cell_voltage\":%.3f,", o->delta_cell_voltage);
    APPEND("\"average_cell_voltage\":%.3f,", o->average_cell_voltage);

    APPEND("\"total_voltage\":%.2f,", o->total_voltage);
    APPEND("\"current\":%.2f,", o->current);
    APPEND("\"total_power\":%.2f,", o->total_power);
    APPEND("\"charging_power\":%.2f,", o->charge_power);
    APPEND("\"discharging_power\":%.2f,", o->discharge_power);

    APPEND("\"mos_temperature\":%.1f,", o->mosfet_temperature);
    APPEND("\"temperature_1\":%.1f,", o->temperature_1);
    APPEND("\"temperature_2\":%.1f,", o->temperature_2);
    if (o->temperature_sensor_count >= 3) {
        APPEND("\"temperature_3\":%.1f,", o->temperature_3);
    }
    if (o->temperature_sensor_count >= 4) {
        APPEND("\"temperature_4\":%.1f,", o->temperature_4);
    }
    if (o->temperature_sensor_count >= 5) {
        APPEND("\"temperature_5\":%.1f,", o->temperature_5);
    }
    APPEND("\"temperature_sensor_count\":%d,", o->temperature_sensor_count);

    APPEND("\"state_of_charge\":%d,", o->state_of_charge);
    APPEND("\"state_of_health\":%d,", o->state_of_health);
    APPEND("\"balance_state\":%d,", o->balance_state);
    APPEND("\"balancing_current\":%.2f,", o->balancing_current);
    APPEND("\"capacity_remaining\":%.1f,", o->capacity_remaining);
    APPEND("\"full_charge_capacity\":%.1f,", o->full_charge_capacity);
    APPEND("\"actual_battery_capacity\":%.1f,", o->actual_battery_capacity);
    APPEND("\"charging_cycles\":%d,", o->charging_cycles);
    APPEND("\"total_charging_cycle_capacity\":%d,", (int)o->total_charging_cycle_capacity);
    APPEND("\"battery_strings\":%d,", o->battery_strings);

    {
        char errtext[160];
        char modestext[96];
        jkbms_errors_to_text(o->errors_bitmask, errtext, sizeof(errtext));
        jkbms_modes_to_text(o->operation_mode_bitmask, modestext, sizeof(modestext));
        APPEND("\"errors_bitmask\":%" PRIu32 ",", o->errors_bitmask);
        APPEND("\"errors\":\"%s\",", errtext);
        APPEND("\"operation_mode_bitmask\":%d,", o->operation_mode_bitmask);
        APPEND("\"operation_mode\":\"%s\",", modestext);
    }

    APPEND("\"balancing_switch\":%s,", o->balancing_switch_on ? "true" : "false");
    APPEND("\"charging_switch\":%s,", o->charging_switch_on ? "true" : "false");
    APPEND("\"discharging_switch\":%s,", o->discharging_switch_on ? "true" : "false");

    /* Protection settings */
    APPEND("\"total_voltage_ovp\":%.2f,", o->total_voltage_ovp);
    APPEND("\"total_voltage_uvp\":%.2f,", o->total_voltage_uvp);
    APPEND("\"cell_ovp\":%.3f,", o->cell_ovp);
    APPEND("\"cell_ovp_recovery\":%.3f,", o->cell_ovp_recovery);
    APPEND("\"cell_ovp_delay\":%.0f,", o->cell_ovp_delay);
    APPEND("\"cell_uvp\":%.3f,", o->cell_uvp);
    APPEND("\"cell_uvp_recovery\":%.3f,", o->cell_uvp_recovery);
    APPEND("\"cell_uvp_delay\":%.0f,", o->cell_uvp_delay);
    APPEND("\"cell_diff_protection\":%.3f,", o->cell_diff_protection);
    APPEND("\"discharging_ocp\":%.0f,", o->discharging_ocp);
    APPEND("\"discharging_ocp_delay\":%.0f,", o->discharging_ocp_delay);
    APPEND("\"charging_ocp\":%.0f,", o->charging_ocp);
    APPEND("\"charging_ocp_delay\":%.0f,", o->charging_ocp_delay);
    APPEND("\"balancing_start_voltage\":%.3f,", o->balance_start_voltage);
    APPEND("\"balance_delta_voltage\":%.3f,", o->balance_delta_voltage);
    APPEND("\"mosfet_ot_protect\":%.0f,", o->mosfet_ot_protect);
    APPEND("\"mosfet_ot_recover\":%.0f,", o->mosfet_ot_recover);
    APPEND("\"battery_ot_protect\":%.0f,", o->battery_ot_protect);
    APPEND("\"battery_ot_recover\":%.0f,", o->battery_ot_recover);
    APPEND("\"battery_temp_diff_protect\":%.0f,", o->battery_temp_diff_protect);
    APPEND("\"charge_ot_protect\":%.0f,", o->charge_ot_protect);
    APPEND("\"discharge_ot_protect\":%.0f,", o->discharge_ot_protect);
    APPEND("\"charge_ut_protect\":%.0f,", o->charge_ut_protect);
    APPEND("\"charge_ut_recover\":%.0f,", o->charge_ut_recover);
    APPEND("\"discharge_ut_protect\":%.0f,", o->discharge_ut_protect);
    APPEND("\"discharge_ut_recover\":%.0f,", o->discharge_ut_recover);

    APPEND("\"current_calibration\":%.3f,", o->current_calibration);
    APPEND("\"low_soc_alarm\":%d,", o->low_soc_alarm);
    APPEND("\"protocol_version\":%d,", o->protocol_version);
    APPEND("\"total_runtime_h\":%.1f,", o->total_runtime_hours);
    APPEND("\"battery_type\":\"%s\",", o->battery_type);
    APPEND("\"device_type\":\"%s\",", o->device_type);
    APPEND("\"software_version\":\"%s\",", o->software_version);
    APPEND("\"hardware_version\":\"%s\",", o->hardware_version);
    APPEND("\"manufacturer\":\"%s\"", o->manufacturer);
    APPEND("}");
#undef APPEND
}

/**
 * Publishes the state topic on change: a message is only sent when the JSON
 * differs from the last successfully published one, or when a heartbeat
 * interval has passed (so consumers see an active feed and the retained
 * message stays recent - it is always a complete snapshot). If the publish
 * itself fails, the change-detection buffer is not updated and the next
 * cycle retries.
 */
void app_mqtt_publish_state(const jkbms_data_t *data, bool link_up)
{
    int64_t now;
    size_t len;
    bool unchanged, heartbeat_due;

    if (!s_connected) {
        return;
    }
    build_json(data, link_up, s_json, sizeof(s_json));

    now = esp_timer_get_time();
    unchanged = s_last_json[0] != '\0' && strcmp(s_last_json, s_json) == 0;
    heartbeat_due = now - s_last_publish_us >=
                    (int64_t)CONFIG_JKBMS_STATE_HEARTBEAT_MS * 1000;
    if (unchanged && !heartbeat_due) {
        return;                                      /* nothing new this cycle */
    }

    if (!publish(s_state_topic, s_json, 1, true)) {
        return;                                      /* not sent: retry next cycle */
    }
    len = strlen(s_json) + 1;                        /* store for change detection */
    memcpy(s_last_json, s_json, len);
    s_last_publish_us = now;
}

/* ------------------------------------------------------------------ */
/* Home Assistant discovery                                             */
/* ------------------------------------------------------------------ */

#if CONFIG_JKBMS_HA_DISCOVERY
typedef struct {
    const char *key;
    const char *name;
    const char *unit;
    const char *dev_class;
} mqtt_sensor_entry_t;

static const mqtt_sensor_entry_t k_sensors[] = {
    { "total_voltage",                 "JK-BMS Total Voltage",           "V",  "voltage" },
    { "current",                       "JK-BMS Current",                 "A",  "current" },
    { "total_power",                   "JK-BMS Total Power",             "W",  "power" },
    { "charging_power",                "JK-BMS Charging Power",          "W",  "power" },
    { "discharging_power",             "JK-BMS Discharging Power",       "W",  "power" },
    { "min_cell_voltage",              "JK-BMS Min Cell Voltage",        "V",  "voltage" },
    { "max_cell_voltage",              "JK-BMS Max Cell Voltage",        "V",  "voltage" },
    { "min_cell_voltage_cell",         "JK-BMS Min Voltage Cell",        "",   NULL },
    { "max_cell_voltage_cell",         "JK-BMS Max Voltage Cell",        "",   NULL },
    { "delta_cell_voltage",            "JK-BMS Delta Cell Voltage",      "V",  "voltage" },
    { "average_cell_voltage",          "JK-BMS Average Cell Voltage",    "V",  "voltage" },
    { "mos_temperature",               "JK-BMS MOS Temperature",         "°C", "temperature" },
    { "temperature_1",                 "JK-BMS Temperature Sensor 1",    "°C", "temperature" },
    { "temperature_2",                 "JK-BMS Temperature Sensor 2",    "°C", "temperature" },
    { "temperature_sensor_count",      "JK-BMS Temperature Sensors",     "",   NULL },
    { "state_of_charge",               "JK-BMS State of Charge",         "%",  "battery" },
    { "capacity_remaining",            "JK-BMS Capacity Remaining",      "Ah", NULL },
    { "full_charge_capacity",          "JK-BMS Full Charge Capacity",    "Ah", NULL },
    { "actual_battery_capacity",       "JK-BMS Actual Battery Capacity", "Ah", NULL },
    { "charging_cycles",               "JK-BMS Charging Cycles",         "",   NULL },
    { "total_charging_cycle_capacity", "JK-BMS Total Cycle Capacity",    "Ah", NULL },
    { "battery_strings",               "JK-BMS Battery Strings",         "",   NULL },
    { "errors_bitmask",                "JK-BMS Errors Bitmask",          "",   NULL },
    { "operation_mode_bitmask",        "JK-BMS Operation Mode Bitmask",  "",   NULL },
    { "total_voltage_ovp",             "JK-BMS Total Voltage OVP",       "V",  "voltage" },
    { "total_voltage_uvp",             "JK-BMS Total Voltage UVP",       "V",  "voltage" },
    { "cell_ovp",                      "JK-BMS Cell OVP",                "V",  "voltage" },
    { "cell_ovp_recovery",             "JK-BMS Cell OVP Recovery",       "V",  "voltage" },
    { "cell_ovp_delay",                "JK-BMS Cell OVP Delay",          "s",  NULL },
    { "cell_uvp",                      "JK-BMS Cell UVP",                "V",  "voltage" },
    { "cell_uvp_recovery",             "JK-BMS Cell UVP Recovery",       "V",  "voltage" },
    { "cell_uvp_delay",                "JK-BMS Cell UVP Delay",          "s",  NULL },
    { "cell_diff_protection",          "JK-BMS Cell Diff Protection",    "V",  "voltage" },
    { "discharging_ocp",               "JK-BMS Discharging OCP",         "A",  "current" },
    { "discharging_ocp_delay",         "JK-BMS Discharging OCP Delay",   "s",  NULL },
    { "charging_ocp",                  "JK-BMS Charging OCP",            "A",  "current" },
    { "charging_ocp_delay",            "JK-BMS Charging OCP Delay",      "s",  NULL },
    { "balancing_start_voltage",       "JK-BMS Balance Start Voltage",   "V",  "voltage" },
    { "balance_delta_voltage",         "JK-BMS Balance Delta Voltage",   "V",  "voltage" },
    { "mosfet_ot_protect",             "JK-BMS MOSFET OT Protection",    "°C", "temperature" },
    { "mosfet_ot_recover",             "JK-BMS MOSFET OT Recovery",      "°C", "temperature" },
    { "battery_ot_protect",            "JK-BMS Battery OT Protection",   "°C", "temperature" },
    { "battery_ot_recover",            "JK-BMS Battery OT Recovery",     "°C", "temperature" },
    { "battery_temp_diff_protect",     "JK-BMS Battery Temp Diff Protection", "°C", "temperature" },
    { "charge_ot_protect",             "JK-BMS Charge OT Protection",    "°C", "temperature" },
    { "discharge_ot_protect",          "JK-BMS Discharge OT Protection", "°C", "temperature" },
    { "charge_ut_protect",             "JK-BMS Charge UT Protection",    "°C", "temperature" },
    { "charge_ut_recover",             "JK-BMS Charge UT Recovery",      "°C", "temperature" },
    { "discharge_ut_protect",          "JK-BMS Discharge UT Protection", "°C", "temperature" },
    { "discharge_ut_recover",          "JK-BMS Discharge UT Recovery",   "°C", "temperature" },
    { "current_calibration",           "JK-BMS Current Calibration",     "A",  "current" },
    { "low_soc_alarm",                 "JK-BMS Low SOC Alarm",           "%",  NULL },
    { "protocol_version",              "JK-BMS Protocol Version",        "",   NULL },
    { "total_runtime_h",               "JK-BMS Total Runtime",           "h",  NULL },
};

static void publish_discovery_sensor(const char *key, const char *name,
                                     const char *unit, const char *dev_class, size_t idx)
{
    char topic[192];
    char uniq[128];
    char payload[DISCOVERY_BUF];

    snprintf(uniq, sizeof(uniq), "jkbms-%s-%s", s_dev_id, key);
    snprintf(topic, sizeof(topic), "homeassistant/sensor/jkbms-%s/jkbms_%s/config", s_dev_id, key);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.%s }}\","
             "\"unique_id\":\"%s\",\"availability_topic\":\"%s\"",
             name, s_state_topic, key, uniq, s_mqtt_available_topic);
    if (unit[0] != '\0') {
        snprintf(payload + strlen(payload), sizeof(payload) - strlen(payload), ",\"unit_of_measurement\":\"%s\"", unit);
    }
    if (dev_class != NULL) {
        snprintf(payload + strlen(payload), sizeof(payload) - strlen(payload), ",\"device_class\":\"%s\"", dev_class);
    }
    snprintf(payload + strlen(payload), sizeof(payload) - strlen(payload),
             ",\"state_class\":\"measurement\",\"device\":{\"identifiers\":[\"jkbms-%s\"],"
             "\"name\":\"JK-BMS (%s)\",\"manufacturer\":\"Jikong\",\"model\":\"\"}}",
             s_dev_id, s_dev_id);
    publish(topic, payload, 1, true);
}

static void publish_discovery(void)
{
    char topic[192];
    char uniq[128];
    char payload[DISCOVERY_BUF];
    int i;

    /* Per-cell voltage sensors */
    for (i = 0; i < JKBM_MAX_CELLS; i++) {
        char key[32];
        char name[48];
        snprintf(key, sizeof(key), "cell_voltage_%d", i + 1);
        snprintf(name, sizeof(name), "JK-BMS Cell Voltage %d", i + 1);
        publish_discovery_sensor(key, name, "V", "voltage", (size_t)i);
    }

    for (i = 0; i < (int)(sizeof(k_sensors) / sizeof(k_sensors[0])); i++) {
        publish_discovery_sensor(k_sensors[i].key, k_sensors[i].name,
                                 k_sensors[i].unit, k_sensors[i].dev_class, (size_t)i);
    }

    /* Binary sensors (as template entities on the state topic) */
    static const char *const binaries[] = { "balancing_switch", "charging_switch", "discharging_switch" };
    static const char *const binary_names[] = { "JK-BMS Balancing", "JK-BMS Charging MOS", "JK-BMS Discharging MOS" };
    static const char *const binary_tpls[] =
        { "balancing_switch", "charging_switch", "discharging_switch" };

    for (i = 0; i < 3; i++) {
        snprintf(uniq, sizeof(uniq), "jkbms-%s-%s", s_dev_id, binaries[i]);
        snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/jkbms-%s/jkbms_%s/config", s_dev_id, binaries[i]);
        snprintf(payload, sizeof(payload),
                 "{\"name\":\"%s\",\"state_topic\":\"%s\",\"availability_topic\":\"%s\","
                 "\"value_template\":\"{{ 'ON' if (value_json.%s is defined and value_json.%s) else 'OFF' }}\","
                 "\"unique_id\":\"%s\",\"device\":{\"identifiers\":[\"jkbms-%s\"],"
                 "\"name\":\"JK-BMS (%s)\",\"manufacturer\":\"Jikong\",\"model\":\"\"}}",
                 binary_names[i], s_state_topic, s_mqtt_available_topic,
                 binary_tpls[i], binary_tpls[i], uniq, s_dev_id, s_dev_id);
        publish(topic, payload, 1, true);
    }

    /* Text sensors */
    static const char *const texts[] = { "errors", "operation_mode", "battery_type", "device_type", "software_version", "manufacturer" };
    static const char *const text_names[] = { "JK-BMS Errors", "JK-BMS Operation Mode", "JK-BMS Battery Type",
                                              "JK-BMS Device Type", "JK-BMS Software Version", "JK-BMS Manufacturer" };

    for (i = 0; i < 6; i++) {
        snprintf(uniq, sizeof(uniq), "jkbms-%s-%s", s_dev_id, texts[i]);
        snprintf(topic, sizeof(topic), "homeassistant/sensor/jkbms-%s/jkbms_%s/config", s_dev_id, texts[i]);
        snprintf(payload, sizeof(payload),
                 "{\"name\":\"%s\",\"state_topic\":\"%s\",\"availability_topic\":\"%s\","
                 "\"value_template\":\"{{ value_json.%s }}\",\"unique_id\":\"%s\","
                 "\"device\":{\"identifiers\":[\"jkbms-%s\"],\"name\":\"JK-BMS (%s)\","
                 "\"manufacturer\":\"Jikong\",\"model\":\"\"}}",
                 text_names[i], s_state_topic, s_mqtt_available_topic, texts[i], uniq,
                 s_dev_id, s_dev_id);
        publish(topic, payload, 1, true);
    }

    ESP_LOGI(TAG, "Home Assistant discovery published (%d cells + base/params/binary/text sensors)", JKBM_MAX_CELLS);
}
#endif /* CONFIG_JKBMS_HA_DISCOVERY */

/* ------------------------------------------------------------------ */
/* MQTT event handling + lifecycle                                      */
/* ------------------------------------------------------------------ */

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "MQTT connected to %s", s_uri_eff);

            /* Availability of the PoE-P4 -> MQTT connection */
            publish(s_mqtt_available_topic, "online", 1, true);

            /* Restore the independently tracked RS485/BMS status */
            publish(s_bms_available_topic,
                    s_bms_available ? "online" : "offline",
                    1, true);

        #if CONFIG_JKBMS_HA_DISCOVERY
            publish_discovery();
        #endif

            /* Force a fresh full state publish after reconnect */
            s_last_json[0] = '\0';
            s_last_publish_us = 0;

            /* OTA trigger subscription (must renew on every (re)connect) */
            (void)esp_mqtt_client_subscribe(s_client, s_ota_topic, 1);
            break;
        case MQTT_EVENT_DATA: {
            /* OTA trigger: the payload of "<prefix>/ota" is an image URL.
             * event->data is NOT NUL-terminated - copy + trim first. */
            if (event->topic_len >= 0 &&
                (size_t)event->topic_len == strlen(s_ota_topic) &&
                strncmp(event->topic, s_ota_topic, (size_t)event->topic_len) == 0) {
                char url[256];
                int n = event->data_len;

                if (n >= (int)sizeof(url)) {
                    n = sizeof(url) - 1;
                }
                if (n > 0 && event->data != NULL) {
                    memcpy(url, event->data, (size_t)n);
                    while (n > 0 && (url[n - 1] == '\n' || url[n - 1] == '\r' ||
                                     url[n - 1] == ' ' || url[n - 1] == '\t')) {
                        n--;
                    }
                    url[n] = '\0';
                    (void)app_ota_start_from_url(url);
                }
            }
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "MQTT disconnected, automatic reconnect is running");
            break;
        case MQTT_EVENT_ERROR:
            if (event->error_handle) {
                ESP_LOGE(TAG, "MQTT error: esp_mqtt transport error=0x%x, esp_tls last_err=0x%x, "
                              "(DNS failure connecting?)",
                         event->error_handle->error_type,
                         event->error_handle->esp_tls_last_esp_err);
            }
            break;
        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGD(TAG, "MQTT before connect");
            break;
        default:
            break;
    }
}

/**
 * mqtt<->mqtts scheme handling: an active TLS checkbox upgrades "mqtt://"
 * to "mqtts://" (same host/port, embedded CA verifies the server), an
 * inactive checkbox downgrades "mqtts://" accordingly. Other schemes pass
 * through unchanged (not written back, so the config keeps the user's URI).
 */
static void effective_uri(const char *src, bool tls, char *dst, size_t dst_cap)
{
    if (tls && strncmp(src, "mqtt://", 7) == 0) {
        snprintf(dst, dst_cap, "mqtts://%s", src + 7);
    } else if (!tls && strncmp(src, "mqtts://", 8) == 0) {
        snprintf(dst, dst_cap, "mqtt://%s", src + 8);
    } else {
        snprintf(dst, dst_cap, "%s", src);
    }
}

/** True if the string contains a space/tab/CR/LF (auth footgun detector). */
static bool contains_whitespace(const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
            return true;
        }
    }
    return false;
}

esp_err_t app_mqtt_init(void)
{
    uint8_t mac[6] = { 0 };
    const app_config_t *cfg = app_config_get();
    esp_mqtt_client_config_t mcfg = { 0 };

    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(s_dev_id, sizeof(s_dev_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_client_id, sizeof(s_client_id), "jkbms-%s", s_dev_id);
    snprintf(s_state_topic, sizeof(s_state_topic), "%s/state", CONFIG_JKBMS_TOPIC_PREFIX);
    snprintf(s_bms_available_topic, sizeof(s_bms_available_topic),
             "%s/available", CONFIG_JKBMS_TOPIC_PREFIX);
    snprintf(s_mqtt_available_topic, sizeof(s_mqtt_available_topic),
             "%s/mqtt_available", CONFIG_JKBMS_TOPIC_PREFIX);
    snprintf(s_ota_topic, sizeof(s_ota_topic), "%s/ota", CONFIG_JKBMS_TOPIC_PREFIX);
    snprintf(s_ota_status_topic, sizeof(s_ota_status_topic),
             "%s/ota/status", CONFIG_JKBMS_TOPIC_PREFIX);

    if (cfg->mqtt_uri[0] == '\0') {
        /* Runtime decision instead of a build option: without a broker URI
         * there is no MQTT client at all (device only serves the web UI). */
        ESP_LOGI(TAG, "MQTT not configured - set the broker in the web UI"
                      " (sending URL updates to '%s' is off as well)", s_ota_topic);
        return ESP_OK;
    }

    /* Strings come from the static config area and must never be mutated:
     * the MQTT client stores pointers to them. */
    effective_uri(cfg->mqtt_uri, cfg->mqtt_tls, s_uri_eff, sizeof(s_uri_eff));
    mcfg.broker.address.uri = s_uri_eff;
    mcfg.credentials.client_id = s_client_id;
    if (cfg->mqtt_user[0] != '\0') {
        mcfg.credentials.username = cfg->mqtt_user;
        if (cfg->mqtt_pass[0] != '\0') {
            mcfg.credentials.authentication.password = cfg->mqtt_pass;
        }
    }
    if (strncmp(s_uri_eff, "mqtts://", 8) == 0) {
#if CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
        /* TEMPORARY insecure mode: TLS still encrypts, but the broker
         * certificate is not validated (embedded ca_cert.pem is a
         * placeholder). Remove once ca_cert.pem holds the real chain. */
        ESP_LOGW(TAG, "MQTTs: TLS server verification DISABLED (temporary)");
#else
        /* The CA certificate embedded at build time is the verification
         * source for the TLS session. */
        mcfg.broker.verification.certificate = (const char *)server_ca_pem_start;
        mcfg.broker.verification.certificate_len =
            (size_t)(server_ca_pem_end - server_ca_pem_start);
        ESP_LOGI(TAG, "MQTTs enabled (CA %u bytes)",
                 (unsigned)mcfg.broker.verification.certificate_len);
#endif
    }

    /* Auth troubleshooting: what is actually attached to the CONNECT
     * (never the password content, only its length/shape). */
    if (cfg->mqtt_user[0] != '\0') {
        ESP_LOGI(TAG, "MQTT auth: user '%s', password %u chars",
                 cfg->mqtt_user, (unsigned)strlen(cfg->mqtt_pass));
        if (contains_whitespace(cfg->mqtt_user) || contains_whitespace(cfg->mqtt_pass)) {
            ESP_LOGW(TAG, "Credentials contain whitespace - re-enter user/password in the web UI");
        }
    } else {
        ESP_LOGI(TAG, "MQTT auth: anonymous");
    }

    mcfg.session.keepalive = 30;
    mcfg.session.last_will.topic = s_mqtt_available_topic;
    mcfg.session.last_will.msg = "offline";
    mcfg.session.last_will.qos = 1;
    mcfg.session.last_will.retain = 1;
    mcfg.buffer.size = 4096;
    mcfg.buffer.out_size = 2048;

    s_client = esp_mqtt_client_init(&mcfg);
    if (s_client == NULL) {
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return ESP_OK;
}

void app_mqtt_start(void)
{
    if (s_client == NULL || s_started) {
        return;
    }
    s_started = true;
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
    ESP_LOGI(TAG, "MQTT client started (broker: %s, id: %s)", s_uri_eff, s_client_id);
}

void app_mqtt_stop(void)
{
    if (s_client == NULL || !s_started) {
        return;
    }
    s_started = false;
    s_connected = false;
    esp_mqtt_client_stop(s_client);
}
