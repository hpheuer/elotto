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

/* Dark time after EVERY run -- baseline, scoring and measurement alike, and
 * independently of focus_mode, so a matched no-focus control differs in the
 * display and nothing else. 350 ms, not 200: conscious noticing smears over
 * ~100-300 ms, so a 200 ms blank lets attention to target N overlap N+1's
 * sampling. The hardware forces it too -- 1000 ms at a 200 ms gap is 83 % duty,
 * past the point where the measurement loop starves the camera extraction task
 * it consumes from and the sustained rate collapses. See sensor.c's history and
 * PLAN.md for the measurements behind both numbers. */
#define RUN_GAP_MS  350

/* The scoring phase's blank. Longer because its RUN is longer (SCORE_SEGMENTS,
 * ~3 s): what starves the extraction task is the DUTY CYCLE, not the window
 * length, so a 3 s run behind the ordinary 350 ms blank would sit at 89.6 %
 * duty — past the cliff, where the achievable window stretches instead of
 * obeying the segment count. ~1000 ms holds scoring at ~75 %, the same ratio
 * the 1 s / 350 ms measurement cycle was tuned to.
 *
 * A phase-specific gap does not break the "uniform gap" rule, and it is worth
 * saying why rather than assuming. That rule had two reasons: an attended and an
 * unattended session must differ in the DISPLAY and nothing else — this gap
 * applies to both equally — and the baseline must be the same instrument as the
 * runs it is subtracted from, down to duty cycle — but the baseline is
 * subtracted from Phase 2 runs, and scoring has no baseline subtraction at all.
 * Baseline and Phase 2 therefore keep RUN_GAP_MS and stay matched to each other.
 *
 * Net cost is a wash against the three 1 s reps this replaces: 3.0 + 1.0 = 4.0 s
 * per number against 3 × 1.38 = 4.14 s. */
#define SCORE_GAP_MS 1000

// The blank between runs, RUN_GAP_MS long.
void run_gap(void);

// The same blank, for a phase that needs a different one (scoring).
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
