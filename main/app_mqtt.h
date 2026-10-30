#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "jkbms.h"

/**
 * Initializes the MQTT client based on the runtime configuration
 * (app_config: broker URI/credentials/TLS are configured in the web UI and
 * stored in the NVS partition 'cfg'). An empty broker URI means MQTT stays
 * completely disabled (no client, publishes are no-ops). Call this before
 * app_mqtt_start(), which should be called once an IP address is available.
 *
 * The broker hostname is resolved via DNS at runtime on every (re)connect.
 */
esp_err_t app_mqtt_init(void);
void app_mqtt_start(void);
void app_mqtt_stop(void);

/**
 * Publishes the current BMS status as JSON to "<prefix>/state" (retained).
 * data may be NULL to publish an "unavailable" state {link_up:false}.
 */
void app_mqtt_publish_state(const jkbms_data_t *data, bool link_up);

/**
 * Publishes the OTA progress/result to "<prefix>/ota/status" (retained).
 * Used internally by app_ota.c.
 */
void app_mqtt_publish_ota_status(const char *status);

/**
 * Publishes the current JK-BMS RS485 availability.
 * The last known state is republished after MQTT reconnects.
 */
void app_mqtt_publish_bms_available(bool available);
