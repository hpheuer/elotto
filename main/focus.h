/* ── Current-item display, pause, gap, session clock ──
 *
 * These four share state: pause_gate() accumulates held time, elapsed_ms_now()
 * subtracts it, and a pause must nudge the gap timer or the break is charged to
 * focus_gap_ms. Panel lit <=> this run's bits are being collected.
 * The session is always unattended `[D66]`; GET /focus feeds the HTML card.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sensor.h"

/* Default intentional blank when a session does not override ?gap=. Live
 * sessions use g_status.gap_ms (set on /start); this is only the compile-time
 * default matching RUN_S_DEFAULT × 0.4. */
#define SCORE_GAP_MS 2000

// The blank between runs, `ms` long (callers pass g_status.gap_ms).
void run_gap_ms(int ms);

/* ── Session clock ─────────────────────────────────────────────────── */

// Start (or restart) the session clock. Paused time is excluded from it.
void session_clock_start(void);

// Milliseconds since session_clock_start(), minus everything spent paused.
int64_t elapsed_ms_now(void);

/* ── The panel ─────────────────────────────────────────────────────── */

// Clear the panel and every timing accumulator. Called once per session.
void focus_reset(void);

// Light the panel for the window whose bits are about to be collected.
void focus_publish(FocusKind kind, const uint8_t *nums, int n,
                   const uint8_t *euro, int n_euro);

// The scoring-phase shorthand: a single candidate number.
void focus_show_number(int value, bool is_euro);

// Blank it. Sampling is over; what follows is dark time.
void focus_off(void);

/* Measured window and gap, in ms. The segment-count-to-window conversion is
 * not stable, so the achievable window has to be read rather than assumed. */
void focus_timing_take(float *win_ms, float *gap_ms);

/* ── Pause ─────────────────────────────────────────────────────────── */

/* Blocks while paused. Held BETWEEN runs only -- state stays `running`. */
void pause_gate(void);

/* ── Event log ─────────────────────────────────────────────────────────
 * What the session is DOING, one line per event (session start/end, sweep,
 * exposure changes, the settle pause after them, soft-down), for the page's
 * log card. A ring in PSRAM, kept across sessions; `seq` is monotonic so the
 * page fetches `GET /loops?ev=1` only when /status `ev_seq` moved.
 * `t_ms` is master uptime — the page converts it the way it converts trip
 * times, now − (uptime_ms − t_ms). */
#define EVLOG_N    48
#define EVLOG_TXT  120
typedef struct {
    uint32_t seq;
    uint32_t t_ms;
    char     txt[EVLOG_TXT];
} EvEntry;

void     evlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t evlog_seq(void);
// Oldest first. Returns the number copied (≤ max).
int      evlog_copy(EvEntry *dst, int max);
