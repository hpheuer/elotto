/* ── Focus panel, pause, run gap and the session clock ──────────────────
 *
 * See focus.h for what this module is and why these four belong together.
 * Moved out of sensor.c on 2026-07-27 with no functional change: the block had
 * zero function calls into the GCP statistics around it, and every static it
 * owns was used only by the functions that moved with it. The only edits the
 * file boundary forced were dropping `static` from the eight entry points and
 * adding session_clock_start(), because sensor.c used to assign s_t0 directly.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "sensor.h"
#include "focus.h"

/* ── Focus display + pause (docs/PLAN_4NODE.md Phase 5) ───────────────────
 *
 * The panel shows what is being measured, WHILE it is being measured. That is
 * the whole content of the experiment: it makes the observer part of the
 * measurement window, as in the original GCP/PEAR protocol. Statistically it
 * changes nothing (z is normalised by √segments at any run length), which is
 * why the session is merely *tagged* rather than analysed differently.
 *
 * The invariant that has to hold is: panel lit ⟺ this run's bits are being
 * collected. So focus_publish() runs immediately before the trigger goes out
 * and focus_off() immediately after the local run returns — the reply wait and
 * the per-run bookkeeping are dark time, and the observer should not spend them
 * attending to a target whose measurement has already finished. */
/* Dark time between targets. Phase 5 asked for this to be *aligned* with
 * overhead that already existed rather than added on top — the estimate was
 * ~190 ms per run spent on the slave round-trip and bookkeeping. Measured on
 * the 4-node array, that overhead is **2.3 ms**: the slaves integrate the same
 * window concurrently and are already answering by the time the master's own
 * run returns, so nodes_collect() costs nothing. The gap therefore has to be
 * paid for, which the plan anticipated ("then pay for it in the run budget"),
 * and `Runs = 850` is what pays for it.
 *
 * It is not only a display concern. At ~100 % duty cycle the measurement loop
 * starves the camera extraction task it consumes from (it runs one priority
 * above it), and the sustained rate collapses from 3.49 to 2.68 Mbit/s —
 * measured, with ring `waits` climbing. The blank period is when the producer
 * gets the CPU back, so the gap buys back most of the throughput it costs.
 *
 * Applied to EVERY run — baseline, scoring and measurement — and independently
 * of focus_mode. Two reasons: a matched no-focus control must differ from an
 * attended session in the display and nothing else, and the baseline has to be
 * the same instrument as the runs it is subtracted from, down to duty cycle.
 *
 * **350 ms, not 200** (user decision, 2026-07-25 — "better safe than sorry").
 * The reason is experimental rather than technical: a wider blank guarantees
 * the measurement is coincident with the *right* target. Conscious noticing is
 * itself smeared over ~100–300 ms, comparable to a 200 ms gap, so at 200 ms the
 * tail of attending to target N can still overlap the start of N+1's sampling —
 * which is precisely the mislabeling this phase exists to avoid, and it is not
 * blur: per-combination z feeds the Stouffer accumulation, so a straddled
 * window credits an effect to an unrelated combination.
 *
 * It is forced by the hardware too. A 1000 ms window at a 200 ms gap is 83 %
 * duty *by construction* — already past the ~72 % cliff above — so no segment
 * count reaches 1000 ms there: every candidate lands in the collapsed regime
 * and stretches to ~1500 ms instead (measured at 16700 and 17000). At the
 * 200 ms gap the reachable window jumps straight from 588 ms to 1494 ms, with
 * nothing in between — not even within ±30 %. 350 ms puts the same 1000 ms
 * window at 74 % duty, back on the flat part, where a count does solve.
 *
 * Shortening the window instead was rejected: the gap is the soft parameter the
 * plan already marked adjustable ("then pay for it in the run budget"), whereas
 * the hold time is the spec, and buying a display number by running the entropy
 * source in a starved regime trades physics for cosmetics — that regime is
 * where PLAN_4NODE's open item 3 lives. */
void run_gap(void)
{
    vTaskDelay(pdMS_TO_TICKS(RUN_GAP_MS));
}

static int64_t s_t0;               // session start
static int64_t s_paused_us;        // total time held by pause, excluded from elapsed
static int64_t s_focus_on_us, s_focus_off_us;
static double  s_win_sum, s_gap_sum;
static uint32_t s_win_n, s_gap_n;
// Which kind the TIMING accumulators are currently describing. Separate from
// g_status.focus.kind, which only exists while the panel is live — the timing
// runs in every session, so it cannot key off a field an unattended session
// never writes. 0xFF = nothing measured yet.
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
     * INSTRUMENT, not of the display: it is set by a segment count whose
     * conversion to milliseconds is not stable (open item 4 — the achievable
     * window moved 1.75x across one afternoon), and per-loop calibration now
     * changes the camera's rate deliberately, which moves it again (§1.5.3).
     * So it has to be measured in every session, attended or not.
     *
     * It used to sit behind the focus_mode guard below, which made
     * focus_win_ms/focus_gap_ms structurally zero in exactly the unattended
     * control sessions the drift most needed watching in. */
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
    // No gap is charged when s_focus_off_us is 0 — that is what keeps a loop's
    // first window from billing the whole calibration + baseline phase, which
    // no window was open for, as inter-run dark time.
    if (s_focus_off_us) { s_gap_sum += (double)(now - s_focus_off_us); s_gap_n++; }
    s_focus_on_us = now;

    if (!g_status.focus_mode) return;      // unattended session: panel stays dark
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
 * Per loop, not per session, and that is the whole point: the window drifts
 * (open item 4 — it crept 474.8 -> 514.4 ms over 1700 runs), and per-loop
 * calibration now moves the camera's rate deliberately, so a session-long mean
 * would average away exactly the effect worth seeing. /status keeps the loop in
 * progress, /loops keeps the completed ones, and the series across loops is the
 * drift record §1.5.3 asks for.
 *
 * Clearing s_focus_off_us also breaks the gap chain across the loop boundary,
 * so the next loop's first window is not billed for the calibration and
 * baseline phases it spent dark. */
void focus_timing_take(float *win_ms, float *gap_ms)
{
    *win_ms = s_win_n ? (float)(s_win_sum / s_win_n / 1000.0) : 0.0f;
    *gap_ms = s_gap_n ? (float)(s_gap_sum / s_gap_n / 1000.0) : 0.0f;
    s_win_sum = s_gap_sum = 0.0;
    s_win_n   = s_gap_n   = 0;
    s_focus_off_us = 0;
}

/* Hold BETWEEN runs. Called where abort_requested is already tested, so the
 * current run always finishes and is kept: stopping mid-run would leave bits
 * sampled while nobody was watching inside a run labelled as attended, which is
 * the one contamination this phase exists to avoid.
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

// Binomial coefficient C(n, r) for small values (max n=15, r=6)
