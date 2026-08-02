/* ── The observer-facing session: focus panel, pause, gap, session clock ──
 *
 * These four things are one module because they share state, not because they
 * are merely similar. pause_gate() accumulates the time held, elapsed_ms_now()
 * subtracts it, and the pause also has to nudge the gap timer or the break gets
 * charged to focus_gap_ms. Splitting them would mean exporting the accumulators.
 *
 * The invariant the whole panel exists to hold: **panel lit <=> this run's bits
 * are being collected.** focus_publish() runs immediately before the trigger
 * goes out and focus_off() immediately after the local run returns; the reply
 * wait and the per-run bookkeeping are dark time. An observer attending to a
 * target whose measurement already finished is the mislabeling this phase was
 * built to prevent.
 *
 * Statistically none of it changes anything -- z is normalised by sqrt(segments)
 * at any run length -- which is why a session is merely TAGGED (?focus=1) rather
 * than analysed differently. Attended and unattended sessions must never be
 * pooled; run matched controls.
 *
 * Split out of sensor.c 2026-07-27 as a pure move. It had no dependency on the
 * GCP statistics at all: not one function call outward.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sensor.h"

/* THE dark time after every run — baseline, scoring and measurement alike
 * (v3: all three phases run the same ~3.4 s window), and independently of
 * focus_mode, so a matched no-focus control differs in the display and
 * nothing else.
 *
 * ~1000 ms because what starves the camera extraction task is the DUTY
 * CYCLE, not the window length: a 3.4 s run behind the old 350 ms blank
 * would sit at ~90 % duty — past the cliff, where the achievable window
 * stretches instead of obeying the segment count. 1000 ms holds the cycle at
 * ~77 % (measured: window 3370 ms, gap 1010 ms, duty 76.9 %, zero stalls),
 * the same ratio the old 1 s / 350 ms cycle was tuned to. It also does the
 * attention job the old 350 ms did with margin: conscious noticing smears
 * over ~100–300 ms, and a 1 s blank cleanly separates target N from N+1. */
#define SCORE_GAP_MS 1000

// The blank between runs, `ms` long (every caller passes SCORE_GAP_MS now).
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

/* Measured window and gap, in ms. Recorded in EVERY session, attended or not:
 * the segment-count-to-window conversion is not stable, so the achievable
 * window has to be read rather than assumed. */
void focus_timing_take(float *win_ms, float *gap_ms);

/* ── Pause ─────────────────────────────────────────────────────────── */

/* Blocks while paused. Held BETWEEN runs only -- state stays `running`, and the
 * permutation and Sigma z resume where they left off, so a tired observer can
 * stop attending without throwing the loop away. */
void pause_gate(void);
