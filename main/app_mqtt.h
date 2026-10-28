#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "jkbms.h"

/**
 * Initializes the MQTT client (broker/credentials/topic prefix are set at
 * build time via menuconfig / sdkconfig.defaults). Call this before
 * app_mqtt_start(), which should be called once a DHCP lease was obtained.
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
