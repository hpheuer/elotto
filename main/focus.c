/* ── Focus panel, pause, run gap and the session clock ──────────────────
 *
 * See focus.h for what this module is and why these four belong together. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "sensor.h"
#include "focus.h"

/* ── Current-item display + pause ────────────────────────────────────────
 *
 * The HTML card shows what is being measured, WHILE it is being measured.
 * Panel lit ⟺ this run's bits are being collected: focus_publish() immediately
 * before the trigger, focus_off() immediately after the local run returns.
 * The session is always unattended; the card is a live readout. */
/* Dark time between targets. Applied to EVERY run — scoring and measurement:
 * the gap is a duty-cycle property of the instrument, not of the display. */
void run_gap_ms(int ms)
{
    if (ms < 0) ms = 0;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static int64_t s_t0;               // session start
static int64_t s_paused_us;        // total time held by pause, excluded from elapsed
static int64_t s_focus_on_us, s_focus_off_us;
static double  s_win_sum, s_gap_sum;
static uint32_t s_win_n, s_gap_n;
// Which kind the TIMING accumulators are currently describing. Separate from
// g_status.focus.kind, which only exists while the panel is live — the timing
// runs whether or not a browser is polling. 0xFF = nothing measured yet.
static uint8_t s_timing_kind = 0xFF;

void session_clock_start(void)
{
    s_t0 = esp_timer_get_time();
}

int64_t elapsed_ms_now(void)
{
    return (esp_timer_get_time() - s_t0 - s_paused_us) / 1000;
}

void focus_reset(void)
{
    memset(&g_status.focus, 0, sizeof(g_status.focus));
    s_focus_on_us = s_focus_off_us = 0;
    s_win_sum = s_gap_sum = 0.0;
    s_win_n = s_gap_n = 0;
    s_timing_kind = 0xFF;
    g_status.focus_win_ms = g_status.focus_gap_ms = 0.0f;
    g_status.paused    = false;
    g_status.paused_ms = 0;
    s_paused_us = 0;
}

/* Put a target on screen. `seq` is bumped LAST and `active` with it, so a
 * reader that sees a given seq sees the numbers that belong to it (the /focus
 * handler re-reads seq to detect the race rather than taking a lock on the
 * measurement path). */
void focus_publish(FocusKind kind, const uint8_t *nums, int n,
                          const uint8_t *euro, int ne)
{
    /* TIMING FIRST, AND UNCONDITIONALLY. The run window is a property of the
     * INSTRUMENT, not of the display — so it has to be measured in every
     * session. */
    int64_t now = esp_timer_get_time();
    // Scoring and measurement are accumulated separately: they are the same
    // 1000 ms window today, but they are different phases and a pooled mean
    // would describe neither if that ever diverges again.
    if (s_timing_kind != (uint8_t)kind) {
        s_timing_kind  = (uint8_t)kind;
        s_win_sum = s_gap_sum = 0.0;
        s_win_n = s_gap_n = 0;
        s_focus_off_us = 0;
    }
    // No gap is charged when s_focus_off_us is 0 — that is what keeps a block's
    // first window from billing the whole sweep + scoring pass, which no window
    // was open for, as inter-run dark time.
    if (s_focus_off_us) { s_gap_sum += (double)(now - s_focus_off_us); s_gap_n++; }
    s_focus_on_us = now;

    FocusState *F = &g_status.focus;
    F->active = 0;
    __sync_synchronize();
    F->kind = (uint8_t)kind;
    F->n    = (uint8_t)n;
    F->ne   = (uint8_t)ne;
    for (int i = 0; i < n  && i < 6; i++) F->nums[i] = nums[i];
    for (int i = 0; i < ne && i < 2; i++) F->euro[i] = euro[i];
    __sync_synchronize();
    F->seq++;
    F->active = 1;
}

void focus_show_number(int value, bool is_euro)
{
    uint8_t v = (uint8_t)value;
    if (is_euro) focus_publish(FOCUS_NUMBER, NULL, 0, &v, 1);
    else         focus_publish(FOCUS_NUMBER, &v, 1, NULL, 0);
}

/* Blank to the fixation mark. Not "nothing": the gaze stays anchored where the
 * next target will appear, and onset is the payload. */
void focus_off(void)
{
    // Close the timing window unconditionally, for the reason in
    // focus_publish(). `s_focus_on_us` is the open/closed flag — cleared here —
    // so the several callers that blank an already-dark panel (end of scoring,
    // finalize) cannot double-count a window that was never open.
    if (s_focus_on_us) {
        int64_t now = esp_timer_get_time();
        s_win_sum += (double)(now - s_focus_on_us);
        s_win_n++;
        s_focus_on_us  = 0;
        s_focus_off_us = now;
        g_status.focus_win_ms = (float)(s_win_sum / s_win_n / 1000.0);
        if (s_gap_n) g_status.focus_gap_ms = (float)(s_gap_sum / s_gap_n / 1000.0);
    }
    g_status.focus.active = 0;
}

/* Take this loop's window/gap means and start a fresh accumulation.
 *
 * Per loop, not per session: the window drifts and per-loop calibration moves
 * the camera's rate deliberately, so a session-long mean would average away
 * exactly the effect worth seeing. /status keeps the loop in progress, /loops
 * keeps the completed ones.
 *
 * Clearing s_focus_off_us also breaks the gap chain across the block boundary,
 * so the next block's first window is not billed for the sweep and scoring it
 * spent dark. */
void focus_timing_take(float *win_ms, float *gap_ms)
{
    *win_ms = s_win_n ? (float)(s_win_sum / s_win_n / 1000.0) : 0.0f;
    *gap_ms = s_gap_n ? (float)(s_gap_sum / s_gap_n / 1000.0) : 0.0f;
    s_win_sum = s_gap_sum = 0.0;
    s_win_n   = s_gap_n   = 0;
    s_focus_off_us = 0;
}

/* Hold BETWEEN runs. Called where abort_requested is already tested, so the
 * current run always finishes and is kept.
 *
 * This is not abort — state stays running, nothing is published, and the
 * permutation index and Σz accumulation resume exactly where they left off.
 * The held time is subtracted from elapsed_ms so the ETA does not absorb the
 * break and the session is not later read as continuous. */
void pause_gate(void)
{
    if (!g_status.paused) return;
    focus_off();                       // panel switches to its paused state
    int64_t p0 = esp_timer_get_time();
    while (g_status.paused && !g_status.abort_requested) {
        vTaskDelay(pdMS_TO_TICKS(100));
        g_status.paused_ms = (s_paused_us + esp_timer_get_time() - p0) / 1000;
    }
    s_paused_us += esp_timer_get_time() - p0;
    g_status.paused_ms  = s_paused_us / 1000;
    // The gap timer would otherwise charge the whole break to the inter-run gap
    // and make focus_gap_ms meaningless.
    s_focus_off_us = esp_timer_get_time();
}

/* ── Event log ───────────────────────────────────────────────────────────
 * PSRAM. Written by the session task, read by the HTTP task:
 * the line is formatted OUTSIDE the lock and only the copy is guarded. */
static EvEntry     *s_ev;
static uint32_t     s_ev_seq;          // entries ever written; slot = seq % N
static portMUX_TYPE s_ev_mux = portMUX_INITIALIZER_UNLOCKED;

void evlog(const char *fmt, ...)
{
    if (!s_ev) {
        s_ev = heap_caps_calloc(EVLOG_N, sizeof(EvEntry), MALLOC_CAP_SPIRAM);
        if (!s_ev) return;
    }
    char txt[EVLOG_TXT];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(txt, sizeof(txt), fmt, ap);
    va_end(ap);
    uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    printf("event: %s\n", txt);

    taskENTER_CRITICAL(&s_ev_mux);
    EvEntry *e = &s_ev[s_ev_seq % EVLOG_N];
    e->seq  = s_ev_seq + 1;
    e->t_ms = t_ms;
    memcpy(e->txt, txt, sizeof(txt));
    s_ev_seq++;
    taskEXIT_CRITICAL(&s_ev_mux);
}

uint32_t evlog_seq(void) { return s_ev_seq; }

int evlog_copy(EvEntry *dst, int max)
{
    if (!s_ev || max <= 0) return 0;
    taskENTER_CRITICAL(&s_ev_mux);
    uint32_t end   = s_ev_seq;
    uint32_t have  = end < EVLOG_N ? end : EVLOG_N;
    if (have > (uint32_t)max) have = (uint32_t)max;
    for (uint32_t i = 0; i < have; i++)
        dst[i] = s_ev[(end - have + i) % EVLOG_N];
    taskEXIT_CRITICAL(&s_ev_mux);
    return (int)have;
}
