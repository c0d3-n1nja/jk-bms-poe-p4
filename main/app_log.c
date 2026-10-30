/* Console log mirror + viewer redirect for the HTTP device log
 * (see app_log.h).
 *
 * A custom esp_log vprintf hook (esp_log_set_vprintf) stores every line in
 * a RAM line ring (fetched by the terminal tab via GET /log/data) and -
 * unless a viewer is currently connected - forwards it to the console
 * vprintf captured at init (USB-Serial-JTAG or UART0, depending on the
 * build), so the serial output keeps working exactly as before.
 *
 * While a viewer polls (heartbeat via app_log_viewer_touch), the console
 * output is paused: the log goes to the web ring only. A 1 s housekeeping
 * timer deactivates the redirect after VIEWER_TIMEOUT_MS without a poll
 * and restores the log level that was active before the viewer session.
 *
 * Concurrency: log calls can arrive from any task; the ring and the
 * viewer state are guarded by a spinlock (ISR safe). Log calls from an
 * ISR context (not allowed by esp_log anyway) are only forwarded. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "app_log.h"

#if CONFIG_JKBMS_LOG_RING_KB > 0

#define VIEWER_TIMEOUT_MS 10000 /* no poll for this long -> back to console */

static vprintf_like_t s_console_vprintf;    /* console output, untouched    */
static app_log_line_t *s_lines;             /* line ring (malloc'd)         */
static size_t s_count;                      /* ring capacity in lines       */
static size_t s_head;                       /* next write position          */
static size_t s_used;                       /* lines currently in the ring  */
static uint32_t s_next_seq = 1;             /* seq of the next line         */
static uint32_t s_last_seen_ms;             /* last viewer poll (heartbeat) */
static bool s_viewer;                       /* redirect active              */
static esp_log_level_t s_saved_level;       /* level before the redirect    */
static esp_timer_handle_t s_timer;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/** True while the redirect should swallow console output. */
static bool viewer_live_locked(void)
{
    return s_viewer &&
           (uint32_t)(esp_log_timestamp() - s_last_seen_ms) < VIEWER_TIMEOUT_MS;
}

static void viewer_deactivate(void)
{
    portENTER_CRITICAL(&s_mux);
    bool was = s_viewer;
    s_viewer = false;
    esp_log_level_t lvl = s_saved_level;
    portEXIT_CRITICAL(&s_mux);

    if (was) {
        esp_log_level_set("*", lvl);
        ESP_LOGI("app_log", "log viewer gone: console output restored (level %d)",
                 (int)lvl);
    }
}

static void viewer_timer_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    bool live = viewer_live_locked();
    portEXIT_CRITICAL(&s_mux);
    if (live) {
        return;
    }
    viewer_deactivate();
}

/** esp_log hook: store the line in the ring; forward to the console
 *  unless a viewer is connected (output redirected to the web). */
static int log_hook_vprintf(const char *format, va_list args)
{
    char line[APP_LOG_LINE_MAX];
    va_list save;
    int r = 0;

    if (xPortInIsrContext() == pdTRUE) {
        /* Log output from an ISR is not supported by esp_log either;
         * pass through without touching the ring. */
        return s_console_vprintf(format, args);
    }

    va_copy(save, args);
    {
        int n = vsnprintf(line, sizeof(line), format, args);

        if (n > 0) {
            size_t len = (n < (int)sizeof(line)) ? (size_t)n
                                                 : (size_t)sizeof(line) - 1;

            if (n >= (int)sizeof(line)) {            /* mark truncation */
                memcpy(line + len - 3, "...", 3);
            }
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                len--;
            }

            portENTER_CRITICAL(&s_mux);
            {
                app_log_line_t *slot = &s_lines[s_head];
                bool esc = false;
                size_t o = 0;

                slot->seq = s_next_seq++;
                /* copy stripped of ANSI colour codes (ESC ... 'm') */
                for (size_t i = 0; i < len && o < sizeof(slot->text); i++) {
                    char c = line[i];

                    if (esc) {
                        if (c == 'm') {
                            esc = false;
                        }
                    } else if (c == 0x1b) {
                        esc = true;
                    } else {
                        slot->text[o++] = c;
                    }
                }
                slot->len = (uint16_t)o;
                s_head = (s_head + 1) % s_count;
                if (s_used < s_count) {
                    s_used++;
                }
            }
            bool to_web = viewer_live_locked();
            portEXIT_CRITICAL(&s_mux);

            if (!to_web) {
                r = s_console_vprintf(format, save);
            }
        }
    }
    va_end(save);
    return r;
}

void app_log_init(void)
{
    if (s_lines != NULL) {
        return;
    }
    s_count = ((size_t)CONFIG_JKBMS_LOG_RING_KB * 1024) / sizeof(app_log_line_t);
    if (s_count < 8) {
        s_count = 8;
    }
    s_lines = malloc(s_count * sizeof(app_log_line_t));
    if (s_lines == NULL) {
        s_count = 0;
        ESP_LOGE("app_log", "log ring: no %u bytes of heap",
                 (unsigned)(8 * sizeof(app_log_line_t)));
        return;
    }
    memset(s_lines, 0, s_count * sizeof(app_log_line_t));
    s_head = 0;
    s_used = 0;

    /* esp_log_set_vprintf returns the currently installed function (the
     * console vprintf) - keep it for the pass-through / restore. */
    s_console_vprintf = esp_log_set_vprintf(log_hook_vprintf);

    const esp_timer_create_args_t targs = {
        .callback = viewer_timer_cb,
        .name = "log_vwr",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, 1000000));
}

void app_log_viewer_touch(void)
{
    bool activate;

    portENTER_CRITICAL(&s_mux);
    s_last_seen_ms = esp_log_timestamp();
    activate = !s_viewer;
    s_viewer = true;
    portEXIT_CRITICAL(&s_mux);

    if (activate) {
        s_saved_level = esp_log_get_default_level();
        ESP_LOGI("app_log", "log viewer connected: console output paused (level was %d)",
                 (int)s_saved_level);
    }
}

size_t app_log_lines_since(uint32_t since, uint32_t *out_last, bool *out_reset,
                           app_log_line_t *out, size_t out_cap)
{
    size_t n = 0;

    if (out == NULL || out_cap == 0) {
        return 0;
    }
    portENTER_CRITICAL(&s_mux);
    {
        uint32_t oldest = s_next_seq - s_used;   /* seq of first line kept */
        uint32_t newest = s_next_seq - 1;

        if (out_last != NULL) {
            *out_last = newest;
        }
        if (out_reset != NULL) {
            *out_reset = (since + 1 < oldest);
        }
        if (s_used > 0 && since < newest) {
            uint32_t s = (since + 1 > oldest) ? since + 1 : oldest;
            size_t idx = (s_head + s_count - (s_next_seq - s)) % s_count;

            for (; s <= newest && n < out_cap; s++, idx = (idx + 1) % s_count) {
                out[n++] = s_lines[idx];
            }
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

bool app_log_set_level(int level)
{
    if (level < (int)ESP_LOG_NONE || level > (int)ESP_LOG_VERBOSE) {
        return false;
    }
    esp_log_level_set("*", (esp_log_level_t)level);
    return true;
}

bool app_log_viewer_active(void)
{
    portENTER_CRITICAL(&s_mux);
    bool a = viewer_live_locked();
    portEXIT_CRITICAL(&s_mux);
    return a;
}

void app_log_clear(void)
{
    portENTER_CRITICAL(&s_mux);
    s_used = 0;
    portEXIT_CRITICAL(&s_mux);
}

#else /* CONFIG_JKBMS_LOG_RING_KB == 0: feature compiled out */

void app_log_init(void)
{
}

void app_log_viewer_touch(void)
{
}

size_t app_log_lines_since(uint32_t since, uint32_t *out_last, bool *out_reset,
                           app_log_line_t *out, size_t out_cap)
{
    (void)since;
    if (out_last != NULL) {
        *out_last = 0;
    }
    if (out_reset != NULL) {
        *out_reset = false;
    }
    return 0;
}

bool app_log_set_level(int level)
{
    (void)level;
    return false;
}

bool app_log_viewer_active(void)
{
    return false;
}

void app_log_clear(void)
{
}

#endif
