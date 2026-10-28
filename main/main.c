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
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "jkbms.h"
#include "app_eth.h"
#include "app_mqtt.h"

static const char *TAG = "jkbms";

#define RS485_UART       ((uart_port_t)CONFIG_JKBMS_RS485_UART_NUM)
#define RS485_BAUD       115200
#define RS485_RX_BUF     (1024 * 2)
#define RS485_TX_BUF     256
#define RS485_RESP_TIMEOUT_MS 150   /* per request; the BMS answers within ~15 ms */
#define RS485_TX_SETTLE_US    2000  /* transceiver turnaround, busy-waited
                                         (vTaskDelay granularity is 10 ms at
                                         CONFIG_FREERTOS_HZ=100 - too coarse
                                         for the RS485 turnaround!) */
#define RS485_INTERFRAME_MS   75    /* bus quiet time before the next request;
                                         the BMS needs a longer pause after its
                                         own response than 5 ms - 75 ms is the
                                         value proven by the ESPHome reference
                                         (modbus command_throttle: 75ms) */
#define JKBM_MAX_CHUNK_WORDS  37u   /* largest proven read length (cell block) */
#define RS485_RX_CAP     512         /* response buffer (max response = 237 bytes) */

#define MODBUS_ADDR      ((uint8_t)CONFIG_JKBMS_MODBUS_SLAVE_ADDR)

static uint8_t s_rx[RS485_RX_CAP];
static bool s_bms_link_up = false;
static int64_t s_last_frame_us = 0;
static jkbms_data_t s_data;
static int s_crc_errors = 0;

/* ------------------------------------------------------------------ */
/* Region table: read in chunks, bisect the chunk size on silence       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t start;
    uint16_t bytes;
    uint16_t chunk_words;   /* cached; smallest used read length */
    uint16_t limit_off;     /* 0 = none; first offset that stayed silent */
    bool (*decode)(uint16_t base, const uint8_t *data, uint16_t len, jkbms_data_t *o);
} region_t;

static region_t s_regions[] = {
    { JKBM_REGION_STATUS_START, JKBM_REGION_STATUS_BYTES, JKBM_MAX_CHUNK_WORDS, 0, jkbms_decode_status_chunk },
    { JKBM_REGION_CONFIG_START, JKBM_REGION_CONFIG_BYTES, JKBM_MAX_CHUNK_WORDS, 0, jkbms_decode_config_chunk },
    { JKBM_REGION_DEV_START,    JKBM_REGION_DEV_BYTES,    JKBM_MAX_CHUNK_WORDS, 0, jkbms_decode_dev_chunk },
    { JKBM_REGION_IDENT_START,  JKBM_REGION_IDENT_BYTES,  JKBM_MAX_CHUNK_WORDS, 0, jkbms_decode_identity_chunk },
};

#define REGION_STATUS 0
#define REGION_CONFIG 1
#define REGION_DEV    2
#define REGION_IDENT  3

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

#ifdef CONFIG_JKBMS_RS485_DEBUG
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
#endif

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
#ifdef CONFIG_JKBMS_RS485_DEBUG
    hexlog("TX", req, frame_len);
#endif
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
#ifdef CONFIG_JKBMS_RS485_DEBUG
            hexlog("RX", resp + got, (size_t)n);
#endif
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
 * Reads one region in chunks. On silence the chunk size is halved and the
 * same offset retried: the BMS answers some (address, size) combinations
 * with silence instead of a Modbus exception. A silent 1-word read marks the
 * offset as region limit (cached) - the rest of the region is skipped in
 * this and the following cycles, but earlier data still flows.
 */
static bool region_read(region_t *r, jkbms_data_t *o)
{
    const uint8_t *d;
    uint16_t off = 0;
    bool any = false;

    while (off < r->bytes) {
        uint16_t remaining = (uint16_t)(r->bytes - off);
        uint16_t words = r->chunk_words;

        if (r->limit_off != 0u && off >= r->limit_off) {
            break;                                   /* known-silent offset reached */
        }
        if ((uint32_t)words * 2u > (uint32_t)remaining) {
            words = (uint16_t)((remaining + 1u) / 2u);
        }
        if (rs485_transaction((uint16_t)(r->start + off), words, s_rx, sizeof(s_rx), &d)) {
            (void)r->decode((uint16_t)(r->start + off), d, (uint16_t)(words * 2u), o);
            off = (uint16_t)(off + words * 2u);
            any = true;
        } else if (words > 1u) {
            r->chunk_words = (uint16_t)(words / 2u);
            ESP_LOGW(TAG, "Region 0x%04x: no answer, chunk reduced to %u words",
                     r->start, r->chunk_words);
        } else {
            if (r->limit_off == 0u) {
                r->limit_off = off;
                ESP_LOGW(TAG, "Region 0x%04x: silent from offset %u - skipping rest",
                         r->start, off);
            }
            break;                                   /* retry after the next poll interval */
        }
    }
    return any;
}

static void poll_once(void)
{
    jkbms_data_t fresh;
    const uint8_t *d;
    bool have_cells = false, have_status = false;

    memset(&fresh, 0, sizeof(fresh));

    if (rs485_transaction(JKBM_REG_CELL_START, JKBM_REG_CELL_WORDS, s_rx, sizeof(s_rx), &d) &&
        jkbms_decode_cell_block(d, JKBM_REG_CELL_BYTES, &fresh)) {
        have_cells = true;
    }

    if (have_cells && region_read(&s_regions[REGION_STATUS], &fresh) &&
        fresh.total_voltage > 1.0f) {
        have_status = true;
    }

    if (!(have_cells && have_status)) {
        return;                                      /* nothing usable this cycle */
    }

    /* Live data complete - publish-worthy; the remaining regions only add
     * extra fields and never invalidate the data above. */
    s_data = fresh;
    s_last_frame_us = esp_timer_get_time();

    (void)region_read(&s_regions[REGION_CONFIG], &s_data);
    (void)region_read(&s_regions[REGION_DEV], &s_data);
    (void)region_read(&s_regions[REGION_IDENT], &s_data);

    if (rs485_transaction(JKBM_REG_PROTO_START, 1, s_rx, sizeof(s_rx), &d)) {
        s_data.protocol_version = d[0];              /* UART1 protocol number (1 = Modbus) */
    }

    if (!s_bms_link_up) {
        s_bms_link_up = true;
        ESP_LOGI(TAG, "JK-BMS online (%d cells, fw %s, model %s)",
                 s_data.cell_count, s_data.software_version, s_data.device_type);
    }

    jkbms_finalize(&s_data);
    publish_current_state();
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
        app_mqtt_start();                                /* idempotent, DNS is resolved at connect time */
    } else {
        app_mqtt_stop();                                 /* on link down: reconnect with new IP/DNS */
    }
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(app_mqtt_init());
    rs485_init();

    xTaskCreate(poll_task, "jk_poll", 4096, NULL, 4, NULL);

    ESP_ERROR_CHECK(app_eth_init(on_eth_app_event));

    ESP_LOGI(TAG, "JK-BMS to MQTT bridge started (JK RS485 Modbus V1.0)");
    ESP_LOGI(TAG, "MQTT broker (built in): %s", CONFIG_JKBMS_MQTT_URI);
    ESP_LOGI(TAG, "MQTT prefix: %s | Polling interval: %d ms | Modbus slave addr: %d",
             CONFIG_JKBMS_TOPIC_PREFIX, CONFIG_JKBMS_POLL_INTERVAL_MS, MODBUS_ADDR);
    ESP_LOGI(TAG, "RS485 RX/TX/DE: %d/%d/%d | 115200 8N1",
             CONFIG_JKBMS_RS485_RX_GPIO, CONFIG_JKBMS_RS485_TX_GPIO, CONFIG_JKBMS_RS485_DE_GPIO);
}
