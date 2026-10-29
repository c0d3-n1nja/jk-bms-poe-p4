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

static const char *TAG = "app_mqtt";

/* Embedded CA cert (main/ca_cert.pem), only used with mqtts:// */
extern const unsigned char server_ca_pem_start[] asm("_binary_ca_cert_pem_start");
extern const unsigned char server_ca_pem_end[]   asm("_binary_ca_cert_pem_end");

#define STATE_JSON_BUF   3072
#define DISCOVERY_BUF    768

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static bool s_started = false;

static char s_dev_id[16];               /* e.g. "ABCDEF123456" */
static char s_client_id[48];
static char s_state_topic[96];
static char s_mqtt_available_topic[96];
static char s_bms_available_topic[96];

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
            ESP_LOGI(TAG, "MQTT connected to %s", CONFIG_JKBMS_MQTT_URI);

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
            break;
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

esp_err_t app_mqtt_init(void)
{
    uint8_t mac[6] = { 0 };
    esp_mqtt_client_config_t cfg = { 0 };

    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(s_dev_id, sizeof(s_dev_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_client_id, sizeof(s_client_id), "jkbms-%s", s_dev_id);
    snprintf(s_state_topic, sizeof(s_state_topic), "%s/state", CONFIG_JKBMS_TOPIC_PREFIX);
    snprintf(s_bms_available_topic, sizeof(s_bms_available_topic),
             "%s/available", CONFIG_JKBMS_TOPIC_PREFIX);
    snprintf(s_mqtt_available_topic, sizeof(s_mqtt_available_topic),
             "%s/mqtt_available", CONFIG_JKBMS_TOPIC_PREFIX);

    cfg.broker.address.uri = CONFIG_JKBMS_MQTT_URI;
    cfg.credentials.client_id = s_client_id;
    if (CONFIG_JKBMS_MQTT_USERNAME[0] != '\0') {
        cfg.credentials.username = CONFIG_JKBMS_MQTT_USERNAME;
        if (CONFIG_JKBMS_MQTT_PASSWORD[0] != '\0') {
            cfg.credentials.authentication.password = CONFIG_JKBMS_MQTT_PASSWORD;
        }
    }
#ifdef CONFIG_JKBMS_MQTT_TLS
    /* Ausgeschaltete bool-Optionen existieren in sdkconfig.h nicht (#ifdef statt if). */
    if (strncmp(CONFIG_JKBMS_MQTT_URI, "mqtts", 5) == 0) {
        /* Das zur Build-Zeit eingebettete CA-Zertifikat dient als Verifizierungsquelle. */
        cfg.broker.verification.certificate = (const char *)server_ca_pem_start;
        cfg.broker.verification.certificate_len = (size_t)(server_ca_pem_end - server_ca_pem_start);
        ESP_LOGI(TAG, "MQTTs enabled (CA %u bytes)", (unsigned)cfg.broker.verification.certificate_len);
    }
#endif

    cfg.session.keepalive = 30;
    cfg.session.last_will.topic = s_mqtt_available_topic;
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.buffer.size = 4096;
    cfg.buffer.out_size = 2048;

    s_client = esp_mqtt_client_init(&cfg);
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
    ESP_LOGI(TAG, "MQTT client started (binding: %s, id: %s)", CONFIG_JKBMS_MQTT_URI, s_client_id);
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
