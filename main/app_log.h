/** Device log: RAM mirror of the console output, served via HTTP.
 *
 *  app_log_init() installs an esp_log vprintf hook (esp_log_set_vprintf).
 *  Every line is appended to an in-RAM line ring with a monotonically
 *  increasing sequence number.
 *
 *  While a log viewer is connected (terminal tab at /log polling
 *  GET /log/data), the log output is REDIRECTED to the ring: the console
 *  (USB-Serial-JTAG or UART0, depending on the build) is paused. As soon
 *  as no viewer polls anymore (10 s timeout), the output goes back to the
 *  console vprintf captured at init.
 *
 *  The log level can be changed at runtime by the viewer
 *  (app_log_set_level -> esp_log_level_set("*", ...)); when the viewer
 *  leaves, the level that was active before the session is restored.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"

#define APP_LOG_LINE_MAX 200    /* per-line cap (esp_log prefix + message) */

/** One log line as stored in the ring (without trailing newline, ANSI
 *  colour codes stripped, truncated with "..." if longer than the cap). */
typedef struct {
    uint32_t seq;                                   /* global line number */
    uint16_t len;                                   /* used bytes in text */
    char text[APP_LOG_LINE_MAX];                    /* NUL not guaranteed */
} app_log_line_t;

/** Installs the log-collector hook. Call once, as early as possible in
 *  app_main, so boot lines are captured. No-op when the feature is
 *  disabled (CONFIG_JKBMS_LOG_RING_KB = 0) or out of memory. */
void app_log_init(void);

/** Viewer heartbeat: called by the /log/data handler on every poll.
 *  Activates the redirect (console -> web ring) on first contact. */
void app_log_viewer_touch(void);

/** Copies every buffered line with seq > since (up to out_cap entries,
 *  oldest first) into out. out_last receives the newest existing sequence
 *  number (the client's next "since"), out_reset is true when lines the
 *  client has not seen were already overwritten (the viewer should clear
 *  its screen and redraw). Returns the number of entries copied. */
size_t app_log_lines_since(uint32_t since, uint32_t *out_last, bool *out_reset,
                           app_log_line_t *out, size_t out_cap);

/** Sets the runtime log level for all tags (0=none .. 5=verbose).
 *  Returns false for out-of-range values. */
bool app_log_set_level(int level);

/** True while the log output is redirected to the web viewer (i.e. the
 *  console is paused). */
bool app_log_viewer_active(void);

/** Empties the ring. Sequence numbers keep counting, so viewers keep
 *  their position. */
void app_log_clear(void);
