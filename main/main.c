/* JK-BMS -> MQTT bridge firmware
 * M5Stack Unit PoE-P4 (U213, legacy revision) + JK-BMS (JK-PB series) via RS485 (SP3485)
 * Protocol: "JK BMS RS485 Modbus V1.0" (protocol 001) - Modbus RTU, FC 0x03, 115200 8N1
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"

#include "jkbms.h"
#include "app_eth.h"
#include "app_mqtt.h"
#include "app_ota.h"
#include "app_config.h"
#include "app_web.h"
#include "app_log.h"

static const char *TAG = "jkbms";

/* Live config reference (assigned once after app_config_init; the config
 * is immutable until the next reboot). */
static const app_config_t *s_cfg = NULL;

#define RS485_UART       ((uart_port_t)CONFIG_JKBMS_RS485_UART_NUM)
#define RS485_BAUD       115200
#define RS485_RX_BUF     (1024 * 2)
#define RS485_TX_BUF     256
#define RS485_RESP_TIMEOUT_MS 150   /* per request; the BMS answers within ~15 ms */
#define RS485_TX_SETTLE_US    2000  /* transceiver turnaround, busy-waited
                                         for precise timing */
#define RS485_INTERFRAME_MS   75    /* bus quiet time enforced after EVERY
                                         transaction: the BMS silently drops
                                         requests that arrive too soon after
                                         its own response (75 ms is the value
                                         proven by the ESPHome reference,
                                         modbus command_throttle: 75ms) */
#define JKBM_MAX_CHUNK_WORDS  37u   /* fixed read length (cell block size);
                                         larger reads are possible, but the
                                         response would exceed the BMS'
                                         comfortable chunking */
#define SILENT_WARN_POLLS     60u   /* consecutive polls with unanswered chunks
                                         before logging a single warning
                                         (~5 min at the default interval) */
#define RS485_RX_CAP     512         /* response buffer (max response = 237 bytes) */

#define MODBUS_ADDR      ((uint8_t)CONFIG_JKBMS_MODBUS_SLAVE_ADDR)

static uint8_t s_rx[RS485_RX_CAP];
static bool s_bms_link_up = false;
static int64_t s_last_frame_us = 0;
static bool s_identity_read = false;    /* 0x1400/0x14B2 are constant: read once */
static bool s_config_read_done = false; /* 0x1000 block: re-read at a lower rate */
static int64_t s_last_config_read_us = 0;
static jkbms_data_t s_data;
static int s_crc_errors = 0;

/* ------------------------------------------------------------------ */
/* Region table: fixed-size chunks, fully re-read every poll cycle       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t start;
    uint16_t bytes;
    uint16_t silent_polls;  /* consecutive polls with an unanswered chunk */
    bool (*decode)(uint16_t base, const uint8_t *data, uint16_t len, jkbms_data_t *o);
} region_t;

static region_t s_regions[] = {
    { JKBM_REGION_STATUS_START, JKBM_REGION_STATUS_BYTES, 0, jkbms_decode_status_chunk },
    { JKBM_REGION_CONFIG_START, JKBM_REGION_CONFIG_BYTES, 0, jkbms_decode_config_chunk },
    { JKBM_REGION_IDENT_START,  JKBM_REGION_IDENT_BYTES,  0, jkbms_decode_identity_chunk },
};

#define REGION_STATUS 0
#define REGION_CONFIG 1
#define REGION_IDENT  2

/* ------------------------------------------------------------------ */
/* RS485 (UART, half duplex, RE/DE actively controlled via GPIO)         */
/* ------------------------------------------------------------------ */

static void rs485_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = RS485_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(RS485_UART, RS485_RX_BUF, RS485_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RS485_UART, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART,
                                 CONFIG_JKBMS_RS485_TX_GPIO,
                                 CONFIG_JKBMS_RS485_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    /* The DE/RE line of the transceiver: output, busy (receiver on) */
    gpio_config_t de_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_JKBMS_RS485_DE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&de_cfg);
    gpio_set_level(CONFIG_JKBMS_RS485_DE_GPIO, 0);
}

/* Raw traffic hexdump; runtime-gated via the web UI debug switch. */
static void hexlog(const char *what, const uint8_t *b, size_t n)
{
    char line[76];
    size_t k;

    ESP_LOGI(TAG, "%s %u bytes:", what, (unsigned)n);
    for (size_t i = 0; i < n; i += 16) {
        k = (size_t)snprintf(line, sizeof(line), "%04zx:", i);
        for (size_t j = i; j < n && j < i + 16; j++) {
            k += (size_t)snprintf(line + k, sizeof(line) - k, " %02x", b[j]);
        }
        ESP_LOGI(TAG, "%s", line);
    }
}

/* ------------------------------------------------------------------ */
/* Modbus transaction: send one FC 0x03 request, read one response      */
/* ------------------------------------------------------------------ */

static bool rs485_transaction(uint16_t start_reg, uint16_t count, uint8_t *resp,
                              uint16_t resp_cap, const uint8_t **data)
{
    uint8_t req[8];
    uint16_t frame_len = jkbms_build_read_request(MODBUS_ADDR, start_reg, count, req);
    uint16_t expected = (uint16_t)(5u + 2u * count);  /* incl. addr/fc/bc + crc16 */
    size_t got = 0;
    int64_t deadline;

    if (expected > resp_cap) {
        return false;
    }

    uart_flush_input(RS485_UART);                    /* discard stale bytes */

    gpio_set_level(CONFIG_JKBMS_RS485_DE_GPIO, 1);   /* transmit enable */
    esp_rom_delay_us(RS485_TX_SETTLE_US);
    if (s_cfg->rs485_debug) {
        hexlog("TX", req, frame_len);
    }
    uart_write_bytes(RS485_UART, (const char *)req, frame_len);
    uart_wait_tx_done(RS485_UART, pdMS_TO_TICKS(100));
    esp_rom_delay_us(RS485_TX_SETTLE_US);            /* allow the last stop bit to run out */
    gpio_set_level(CONFIG_JKBMS_RS485_DE_GPIO, 0);   /* receiving mode */

    deadline = esp_timer_get_time() + (int64_t)RS485_RESP_TIMEOUT_MS * 1000;
    while (got < expected) {
        int n;
        if (esp_timer_get_time() >= deadline) {
            break;
        }
        n = uart_read_bytes(RS485_UART, resp + got, expected - got, pdMS_TO_TICKS(20));
        if (n > 0) {
            if (s_cfg->rs485_debug) {
                hexlog("RX", resp + got, (size_t)n);
            }
            got += (size_t)n;
        }
    }
    bool ok = false;

    if (got >= 5u && resp[1] == 0x83u) {
        /* Modbus exception response (FC 0x83): byte 2 holds the exception code */
        ESP_LOGW(TAG, "Modbus exception 0x%02x for start=0x%04x count=%u",
                 resp[2], start_reg, count);
    } else if (got != expected) {
        /* timeout - resync */
    } else if (!jkbms_parse_response(resp, (uint16_t)got, MODBUS_ADDR, count, data)) {
        s_crc_errors++;
        if (s_crc_errors % 20 == 1) {
            ESP_LOGW(TAG, "Invalid Modbus response (addr/len/CRC); errors=%d", s_crc_errors);
        }
    } else {
        ok = true;
    }

    uart_flush_input(RS485_UART);
    /* Bus quiet time after EVERY transaction: the BMS silently drops requests
     * that arrive too soon after its own response (~4 ms is too fast, 75 ms
     * is proven). Without this, success and failure strictly alternate. */
    vTaskDelay(pdMS_TO_TICKS(RS485_INTERFRAME_MS));
    return ok;
}

/* ------------------------------------------------------------------ */
/* Polling cycle                                                        */
/* ------------------------------------------------------------------ */

static void publish_current_state(void)
{
    app_mqtt_publish_state(&s_data, s_bms_link_up);
}

/**
 * Reads one region in fixed-size chunks (JKBM_MAX_CHUNK_WORDS, last chunk
 * truncated to the remainder). A chunk that gets no answer is accepted
 * silently: its registers keep their previous values and are simply read
 * again on the next poll - there is no retry within the same cycle and the
 * chunk size never changes. If a region stays partially silent for
 * SILENT_WARN_POLLS consecutive polls, one warning is logged (covers a
 * permanent failure; sporadic errors stay completely silent).
 * Returns true if at least one chunk was read.
 */
static bool region_read(region_t *r, jkbms_data_t *o)
{
    const uint8_t *d;
    uint16_t off = 0;
    bool any = false, all_ok = true;

    while (off < r->bytes) {
        uint16_t remaining = (uint16_t)(r->bytes - off);
        uint16_t words = JKBM_MAX_CHUNK_WORDS;

        if ((uint32_t)words * 2u > (uint32_t)remaining) {
            words = (uint16_t)((remaining + 1u) / 2u);
        }
        if (rs485_transaction((uint16_t)(r->start + off), words, s_rx, sizeof(s_rx), &d)) {
            (void)r->decode((uint16_t)(r->start + off), d, (uint16_t)(words * 2u), o);
            any = true;
        } else {
            all_ok = false;                          /* silently skipped */
        }
        off = (uint16_t)(off + words * 2u);
    }

    if (all_ok) {
        r->silent_polls = 0;
    } else if (++r->silent_polls == SILENT_WARN_POLLS) {
        ESP_LOGW(TAG, "Region 0x%04x: chunks unanswered for %u polls in a row",
                 r->start, (unsigned)r->silent_polls);
    }
    return any;
}

static void poll_once(void)
{
    const uint8_t *d;
    int64_t now_us;
    bool have_cells, have_status;

    /* The decoders write directly into s_data: fields covered by a chunk
     * that got no answer keep their previous value until the next poll. */
    have_cells = rs485_transaction(JKBM_REG_CELL_START, JKBM_REG_CELL_WORDS,
                                   s_rx, sizeof(s_rx), &d) &&
                 jkbms_decode_cell_block(d, JKBM_REG_CELL_BYTES, &s_data);

    if (!have_cells) {
        return;                                      /* keep last published state */
    }

    /* Mode bits are OR-ed in by the status decoder - start fresh per cycle */
    s_data.operation_mode_bitmask = 0;

    have_status = region_read(&s_regions[REGION_STATUS], &s_data) &&
                  s_data.total_voltage > 1.0f;

    if (!have_status) {
        return;                                      /* keep last published state */
    }

    s_last_frame_us = esp_timer_get_time();

    /* Protection parameters/switches change rarely: re-read them at most
     * once per JKBMS_CONFIG_READ_INTERVAL_MS (the first cycle always reads
     * them). In between, the last known values remain in s_data. */
    now_us = esp_timer_get_time();
    if (!s_config_read_done ||
        now_us - s_last_config_read_us >=
            (int64_t)CONFIG_JKBMS_CONFIG_READ_INTERVAL_MS * 1000) {
        (void)region_read(&s_regions[REGION_CONFIG], &s_data);
        s_last_config_read_us = now_us;
        s_config_read_done = true;
    }

    if (!s_identity_read) {
        /* Identity strings and the protocol number are constant - read them
         * once; until both answer, the attempt repeats on every cycle. */
        if (region_read(&s_regions[REGION_IDENT], &s_data) &&
            rs485_transaction(JKBM_REG_PROTO_START, 1, s_rx, sizeof(s_rx), &d)) {
            s_data.protocol_version = d[0];          /* UART1 protocol number (1 = Modbus) */
            s_identity_read = true;
        }
    }

    if (!s_bms_link_up) {
        s_bms_link_up = true;

        ESP_LOGI(TAG, "JK-BMS online (%d cells, fw %s, model %s)",
                 s_data.cell_count, s_data.software_version,
                 s_data.device_type);

        app_mqtt_publish_bms_available(true);
    }

    jkbms_finalize(&s_data);
    publish_current_state();

    /* First healthy poll cycle after boot: confirm the running OTA image
     * (cancels the bootloader rollback-pending state; idempotent). */
    app_ota_confirm_running_image();
}

static void poll_task(void *arg)
{
    const int64_t no_resp_us = (int64_t)CONFIG_JKBMS_NO_RESPONSE_TIMEOUT_MS * 1000;

    while (1) {
        poll_once();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_JKBMS_POLL_INTERVAL_MS));

        if (s_bms_link_up && esp_timer_get_time() - s_last_frame_us > no_resp_us) {
            s_bms_link_up = false;
            ESP_LOGW(TAG, "No response from the BMS - going offline");
            app_mqtt_publish_bms_available(false);
            publish_current_state();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Ethernet/MQTT glue code                                              */
/* ------------------------------------------------------------------ */

static void on_eth_app_event(app_eth_event_t event)
{
    if (event == APP_ETH_EVENT_GOT_IP) {
        /* Console visibility of the running version next to the network
         * data (the serial debug console shows the same on every boot). */
        {
            const app_eth_netinfo_t *ni = app_eth_get_netinfo();
            const esp_app_desc_t *app = esp_app_get_description();
            ESP_LOGI(TAG, "Firmware %s | IP %s netmask %s gw %s | DNS %s backup %s",
                     app->version,
                     ni->have_ip ? ni->ip : "(none)",
                     ni->have_ip ? ni->netmask : "-",
                     ni->have_ip ? ni->gateway : "-",
                     ni->have_ip ? ni->dns : "-",
                     ni->have_ip ? ni->dns_backup : "-");
        }
        app_ota_trigger_version_check(); /* starts the checker on first IP */
        app_web_start();                     /* web UI/live config needs an IP */
        app_mqtt_start();                    /* idempotent, DNS resolved at connect time */
    } else {
        app_web_stop();                      /* also on link down: restart after re-IP */
        app_mqtt_stop();                     /* reconnect with new IP/DNS */
    }
}

/* ------------------------------------------------------------------ */
/* Config-reset button: hold >= 10 s -> erase config partition, reboot   */
/* ------------------------------------------------------------------ */

#define RESET_HOLD_MS  10000
#define RESET_POLL_MS  30

/**
 * Recovery path for an unreachable device (e.g. a static IP that no longer
 * fits the network, or a forgotten web password): holding the button for
 * at least 10 seconds erases the configuration partition and reboots into
 * the defaults - DHCP, unchanged RS485 values, web UI without password and
 * MQTT off. The bridge is then reachable only via the address it gets from
 * the DHCP server (router lease table). Erasing NVS here is safe: the
 * next boot re-initializes (formats) the partition.
 */
static void reset_button_task(void *arg)
{
#if CONFIG_JKBMS_RESET_BUTTON_GPIO >= 0
    const gpio_num_t pin = (gpio_num_t)CONFIG_JKBMS_RESET_BUTTON_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << (int)pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,    /* idle high, pressed = low */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    (void)arg;
    gpio_config(&io);
    ESP_LOGI(TAG, "Config reset button on GPIO %d (hold >= %d s)",
             (int)pin, RESET_HOLD_MS / 1000);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RESET_POLL_MS));
        if (gpio_get_level(pin) == 0) {
            int64_t start = esp_timer_get_time();

            ESP_LOGI(TAG, "Reset button pressed (GPIO %d)", (int)pin);
            while (gpio_get_level(pin) == 0 &&
                   esp_timer_get_time() - start < RESET_HOLD_MS * 1000) {
                vTaskDelay(pdMS_TO_TICKS(RESET_POLL_MS));
            }
            if (gpio_get_level(pin) == 0) {  /* still held after the timeout */
                ESP_LOGW(TAG, "Reset button held for %d s -> factory reset",
                         RESET_HOLD_MS / 1000);
                app_config_factory_reset();
                esp_restart();               /* never returns */
            }
            ESP_LOGI(TAG, "Reset button released below %d s - config kept",
                     RESET_HOLD_MS / 1000);
        }
    }
#else
    (void)arg;
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
#endif
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    app_log_init();     /* first of all: HTTP device-log mirror (captures boot lines) */

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_config_init();                       /* dedicated 'cfg' NVS partition */
    s_cfg = app_config_get();                /* immutable until next reboot */

    ESP_ERROR_CHECK(app_mqtt_init());        /* prepared; started once IP is up */
    rs485_init();

    xTaskCreate(poll_task, "jk_poll", 4096, NULL, 4, NULL);
    xTaskCreate(reset_button_task, "cfg_reset", 3072, NULL, 2, NULL);

    ESP_ERROR_CHECK(app_eth_init(on_eth_app_event)); /* web server starts on GOT_IP */

    {
        /* Which image is running and from which flash slot (OTA visibility) */
        const esp_app_desc_t *app = esp_app_get_description();
        ESP_LOGI(TAG, "App %s %s running from partition %s",
                 app->project_name, app->version,
                 esp_ota_get_running_partition()->label);
        ESP_LOGI(TAG, "Update URL: %s (manual URL from web UI has priority)",
                 app_ota_effective_url()[0] != '\0' ? app_ota_effective_url() : "(none)");
    }

    ESP_LOGI(TAG, "JK-BMS to MQTT bridge started (JK RS485 Modbus V1.0)");
    ESP_LOGI(TAG, "MQTT: %s%s | prefix: %s",
             s_cfg->mqtt_uri[0] != '\0' ? s_cfg->mqtt_uri : "(not configured - web UI)",
             s_cfg->mqtt_tls ? " +TLS" : "",
             CONFIG_JKBMS_TOPIC_PREFIX);
    ESP_LOGI(TAG, "Poll interval: %d ms | Modbus slave addr: %d | RS485 debug: %s",
             CONFIG_JKBMS_POLL_INTERVAL_MS, MODBUS_ADDR, s_cfg->rs485_debug ? "on" : "off");
    ESP_LOGI(TAG, "RS485 RX/TX/DE: %d/%d/%d | 115200 8N1",
             CONFIG_JKBMS_RS485_RX_GPIO, CONFIG_JKBMS_RS485_TX_GPIO, CONFIG_JKBMS_RS485_DE_GPIO);
    ESP_LOGI(TAG, "Config web UI: http://<device-ip>/ (%s)",
             s_cfg->web_password[0] != '\0' ? "password protected" : "open");
#if CONFIG_JKBMS_RESET_BUTTON_GPIO >= 0
    ESP_LOGI(TAG, "Holding the button on GPIO %d for 10 s resets the configuration",
             CONFIG_JKBMS_RESET_BUTTON_GPIO);
#endif
}
