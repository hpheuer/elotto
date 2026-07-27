#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "sensor.h"
#include "camera.h"
#include "gcp.h"
#include "nodes.h"
#include "elotto_link.h"

ElottoStatus g_status = { .state = ELOTTO_IDLE };

// Per-combination running Σz across loops (cumulative / Stouffer ranking mode)
static double s_zsum[NUM_RUNS];

// Random measurement order for the current loop (Fisher–Yates, rebuilt per
// loop) — decouples slow source drift from fixed combination indices, so drift
// cannot accumulate coherently on specific combinations across loops
static uint16_t s_perm[NUM_RUNS];

/* Administrative randomness — measurement ORDER, never measured data.
 *
 * The Fisher–Yates shuffle needs thousands of values per loop and camera
 * entropy is rate-limited, so drawing them from the camera would stall the
 * session for bits that never enter a z-score. The generator is instead SEEDED
 * from the camera once per session and run forward arithmetically. Nothing here
 * touches the on-chip TRNG — it is not present in this firmware at all. */
static uint32_t s_prng = 0x9E3779B9u;

/* Not static: nodes.c seeds the link's sequence number from it. One generator
 * with one state, shared exactly as it was when both callers lived in this
 * file — a second instance would be a second stream, which is not what any of
 * this wants. */
uint32_t fast_rng(void)                        /* xorshift32 */
{
    uint32_t x = s_prng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return (s_prng = x);
}

static void prng_seed(void)
{
    uint32_t w = 0;
    if (!camera_read_word(&w)) w = 0;          // camera not streaming yet
    s_prng = w ^ (uint32_t)esp_timer_get_time();
    if (!s_prng) s_prng = 0x9E3779B9u;         // xorshift32 must never sit at 0
}

#define SEGMENT_BITS   200                     // 6 words + 8 bits, per z segment

/* Segments per run (PLAN_4NODE Phase 1 + 5).
 *
 * Phase 5 made the run length the *display* window: the Focus panel holds a
 * target for exactly as long as its bits are collected, so the hold time is not
 * a delay bolted onto a run — it IS the run. Padding with a vTaskDelay would
 * cut the bit rate for nothing.
 *
 * **ONE window for every focus display: 1000 ms** (user decision, 2026-07-25).
 * The spec originally asked for 1000 ms per candidate number and 500 ms per
 * draw; the 500 ms was dropped so that anything the observer is asked to attend
 * to gets the same, longer look. That collapses what was briefly a
 * phase-dependent count back to one constant per source — scoring, measurement
 * and baseline all run the same length — and the two "SCORE/MEAS" pairs are
 * gone with it. The *wire* still carries the count (see slave_trigger), which
 * stays worthwhile: it is what makes the length impossible for a node to get
 * wrong, whether or not it currently varies.
 *
 * The camera counts are CALIBRATED ON HARDWARE, not derived on paper — the
 * Phase 5 gate insists on that, and the paper answer was wrong by 60 %.
 *
 * What makes the paper answer wrong is that the sustained rate is not a
 * constant: the measurement loop runs one priority ABOVE the camera extraction
 * task it consumes from, so the harder it consumes the more it starves its own
 * producer. Measured on the live 4-node array (2026-07-25, all four on camera,
 * a client polling /focus at 10 Hz), per-cycle sustained rate against duty
 * cycle (run / (run + gap)):
 *
 *      n=6350   run  277 ms   duty 57 %   2.63 Mbit/s
 *      n=11600  run  588 ms   duty 72 %   2.82 Mbit/s
 *      n=17000  run 1519 ms   duty 88 %   1.98 Mbit/s   <- collapsed
 *
 * So it is flat near 2.7 Mbit/s and falls off a cliff somewhere past ~72 %, at
 * which point longer runs feed back into a slower producer and get longer
 * still. None of this is visible in Phase 0's 3.49 Mbit/s idle figure. The
 * counts below are solved from the flat part — n = rate × (window + gap) —
 * which also keeps the source out of the starved regime, where PLAN_4NODE's
 * open item 3 (camera bias degrading under sustained load) lives.
 *
 * This is the other reason RUN_GAP_MS is not merely cosmetic: the gap is when
 * the producer gets the CPU back, so it buys back most of what it costs.
 *
 * z stays N(0,1) at any run length because it is normalised by √segments; the
 * length only sets granularity, and statistical power per second is
 * rate-limited either way. */
#define CAM_SEGMENTS   11950          // 2.4 Mbit/run ≈ 1000 ms (measured: 1027 ms
                                      // at the 350 ms gap, +2.7 % of target)


// Called once per session, after discovery, before any measurement.
static void camera_source_begin(void)
{
    g_status.noise_stalled = false;
    g_status.fault[0]      = '\0';
    prng_seed();
    if (!camera_is_ready())
        node_camera_failed(0, "not streaming at session start");
}

static const char *p_label(double absZ)
{
    if (absZ > 3.29) return "p&lt;0.001";
    if (absZ > 2.58) return "p&lt;0.01";
    if (absZ > 1.96) return "p&lt;0.05";
    if (absZ > 1.28) return "p&lt;0.10";
    return "n.s.";
}

/* Segments for one run — one source, so one count, and it travels on the wire
 * so a slave cannot disagree about it. */
static int segments_for(void) { return CAM_SEGMENTS; }

/* One run, via the shared primitive in components/elotto_gcp — the same object
 * code the slaves run, so no node can compute z differently from another.
 *
 * NULL yield callback: the master aborts between runs, never inside one, so
 * there is nothing to poll mid-run. A false return means the run produced no
 * usable z and the caller must not score it — a short run is not a small run,
 * its z would be normalised by a √segments it never reached. */
static bool gcp_zscore_ok(int nseg, double *out)
{
    return gcp_zscore_raw(nseg, NULL, out) == GCP_OK;
}

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
#define RUN_GAP_MS  350

static void run_gap(void)
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

static int64_t elapsed_ms_now(void)
{
    return (esp_timer_get_time() - s_t0 - s_paused_us) / 1000;
}

static void focus_reset(void)
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
static void focus_publish(FocusKind kind, const uint8_t *nums, int n,
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

static void focus_show_number(int value, bool is_euro)
{
    uint8_t v = (uint8_t)value;
    if (is_euro) focus_publish(FOCUS_NUMBER, NULL, 0, &v, 1);
    else         focus_publish(FOCUS_NUMBER, &v, 1, NULL, 0);
}

/* Blank to the fixation mark. Not "nothing": the gaze stays anchored where the
 * next target will appear, and onset is the payload. */
static void focus_off(void)
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
static void focus_timing_take(float *win_ms, float *gap_ms)
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
static void pause_gate(void)
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
static int comb(int n, int r)
{
    if (r < 0 || r > n) return 0;
    if (r == 0) return 1;
    if (r > n - r) r = n - r;
    int res = 1;
    for (int i = 0; i < r; i++)
        res = res * (n - i) / (i + 1);
    return res;
}

// k-th combination (0-based, lexicographic) from sorted pool[0..n-1], r elements
static void nth_combination(const uint8_t *pool, int n, int r, int k, uint8_t *out)
{
    int start = 0;
    for (int i = 0; i < r; i++) {
        for (int j = start; j <= n - (r - i); j++) {
            int c = comb(n - 1 - j, r - 1 - i);
            if (k < c) {
                out[i] = pool[j];
                start = j + 1;
                break;
            }
            k -= c;
        }
    }
}

// Scores each number 1..max_val with exactly ONE node-combined run (÷√k, like
// Phase 2), sweeping the numbers in random order, then picks the top pool_size
// (sorted ascending). The pool is locked for the whole cumulative session, so
// this is where selection confidence matters most — and one run per number is
// the weakest this has ever been: per-number SE = 1/√k over k nodes, i.e. 0.50
// at four nodes, against 0.22 for the 5 reps it replaced and 1.0 for a single
// master-only run.
//
// Stated as a consequence rather than buried, because it is a real cost. It
// changes only WHICH numbers enter the pool, never the Phase-2 statistics
// measured on them — the same caveat the old SCORE_REPS always carried. A
// session whose pool choice must be trusted on its own wants several full
// random passes; it must NOT go back to repeats in place, which is what broke
// the Focus display (see score_and_build_pool).
static double score_one_run(void);   // forward (defined after the slave link block)

/* ── Attended pool confirmation ────────────────────────────────────────
 * The session parks at PHASE_POOL_CONFIRM and spins here until the operator
 * answers through POST /pool. One volatile word carries the verdict, so the
 * webserver task and elotto_task need no lock: the handler writes the selection
 * first and the action word last, and the session reads the action word first. */
static volatile PoolAction s_pool_act = POOL_WAIT;
static uint8_t s_pool_sel_main[POOL_MAIN_49];
static uint8_t s_pool_sel_euro[POOL_EURO_12];
static int     s_pool_sel_nm, s_pool_sel_ne;

bool elotto_pool_reply(PoolAction act,
                       const uint8_t *main_sel, int n_main,
                       const uint8_t *euro_sel, int n_euro)
{
    if (g_status.phase != PHASE_POOL_CONFIRM || g_status.state != ELOTTO_RUNNING)
        return false;
    if (n_main > POOL_MAIN_49) n_main = POOL_MAIN_49;
    if (n_euro > POOL_EURO_12) n_euro = POOL_EURO_12;
    for (int i = 0; i < n_main; i++) s_pool_sel_main[i] = main_sel[i];
    for (int i = 0; i < n_euro; i++) s_pool_sel_euro[i] = euro_sel[i];
    s_pool_sel_nm = n_main;
    s_pool_sel_ne = n_euro;
    s_pool_act    = act;          // published LAST — see note above
    return true;
}

// `euro_pool` selects the bonus-number pool; it only reaches the Focus panel,
// which styles a euro candidate differently from a main one.
/* `keep`/`n_keep` implement "Select more": those numbers are already chosen and
 * are OMITTED from this scoring pass entirely — they keep the measurement that
 * put them in the pool rather than being re-measured, and they occupy the first
 * n_keep slots. Everything else is scored afresh and the best fill what is left.
 * Numbers the operator unchecked are therefore back in contention: this is a new
 * measurement, not a veto list. Pass keep = NULL for the ordinary first pass. */
static void score_and_build_pool(int max_val, int pool_size, uint8_t *pool,
                                 bool euro_pool,
                                 const uint8_t *keep, const float *keep_z,
                                 int n_keep, float *out_z)
{
    double scores[51] = {0};
    bool   skip[51]   = {false};
    if (n_keep > pool_size) n_keep = pool_size;
    for (int i = 0; i < n_keep; i++)
        if (keep[i] >= 1 && keep[i] <= max_val) skip[keep[i]] = true;

    /* One run per candidate number, in a fresh random order — every number
     * exactly once, no repeats (user decision, 2026-07-25).
     *
     * This replaces SCORE_REPS consecutive runs of the *same* number, which the
     * Focus panel made untenable: five reps meant the display did not change
     * for ~6.9 s, and onset — the change — is the payload. A random sweep keeps
     * every window a genuine onset and stops the observer anticipating the next
     * target, for the same reason Phase 2 measures combinations in a permuted
     * order.
     *
     * fast_rng() deliberately, not the camera: measurement order is
     * administrative randomness, not measured data, and must not spend
     * rate-limited camera entropy. */
    uint8_t order[51];
    int     n_order = 0;
    for (int i = 1; i <= max_val; i++)
        if (!skip[i]) order[n_order++] = (uint8_t)i;
    for (int i = n_order - 1; i > 0; i--) {
        int j = (int)(fast_rng() % (uint32_t)(i + 1));
        uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
    }
    /* The kept numbers are not measured, so they must not be counted either —
     * otherwise the progress bar stalls short of its total for the whole pass.
     * ADDS rather than assigns: a Eurojackpot loop calls this twice (50 main,
     * then 12 bonus) and the two are one progress bar, not two. Assigning here
     * made the bonus pass reset a full bar back to 0/12. The caller zeroes the
     * pair before the first pass of a group. */
    g_status.scoring_total += n_order;

    for (int idx = 0; idx < n_order; idx++) {
        if (g_status.abort_requested) return;
        pause_gate();
        if (g_status.abort_requested) return;
        int k = order[idx];
        // On screen before the run starts and until it ends — display and
        // bits cover the same interval, or the panel is decoration.
        focus_show_number(k, euro_pool);
        scores[k] = score_one_run();
        g_status.scoring_done++;
        // Scoring never updated the clock, so elapsed_ms (and with it the ETA)
        // froze for the whole phase. That was survivable when scoring was a
        // preamble; Phase 5 makes it attended screen time with a pause button,
        // so the clock has to run here too.
        g_status.elapsed_ms = elapsed_ms_now();
        run_gap();
    }
    /* The kept numbers take the first slots, carrying the score that chose them
     * (they were not re-measured, so there is no new one to carry). */
    /* A kept number was never measured this pass, so scores[] holds 0 for it.
     * Fold the score it was originally chosen on back in, so one array carries
     * every pool member's score regardless of which pass produced it. */
    for (int i = 0; i < n_keep; i++)
        if (keep[i] >= 1 && keep[i] <= max_val)
            scores[keep[i]] = keep_z ? (double)keep_z[i] : 0.0;

    bool used[51] = {false};
    for (int i = 0; i < n_keep; i++) {
        pool[i] = keep[i];
        if (keep[i] >= 1 && keep[i] <= max_val) used[keep[i]] = true;
    }
    for (int i = n_keep; i < pool_size; i++) {
        int b = 0; double bs = -1e18;
        for (int j = 1; j <= max_val; j++)
            if (!used[j] && !skip[j] && scores[j] > bs) { b = j; bs = scores[j]; }
        pool[i] = (uint8_t)b;
        if (b) used[b] = true;
    }
    // Sort ascending (for consistent combination enumeration).
    for (int i = 1; i < pool_size; i++) {
        uint8_t key = pool[i]; int j = i - 1;
        while (j >= 0 && pool[j] > key) { pool[j+1] = pool[j]; j--; }
        pool[j+1] = key;
    }
    // Scores follow the sorted pool, so slot i always describes pool[i].
    if (out_z)
        for (int i = 0; i < pool_size; i++) {
            int k = pool[i];
            out_z[i] = (k >= 1 && k <= max_val) ? (float)scores[k] : 0.0f;
        }
}

/* Look up each selected number's score in the pool currently on display, so a
 * retained number carries the measurement that put it there. A number not found
 * (which should not happen — the UI can only check what it was shown) scores 0,
 * which simply makes it the first to be replaced by a later pass. */
static void pool_scores_for(const uint8_t *sel, int n_sel,
                            const uint8_t *cur, const float *cur_z, int n_cur,
                            float *out)
{
    for (int i = 0; i < n_sel; i++) {
        out[i] = 0.0f;
        for (int j = 0; j < n_cur; j++)
            if (cur[j] == sel[i]) { out[i] = cur_z[j]; break; }
    }
}

/* How long the session will hold at PHASE_POOL_CONFIRM with nobody answering.
 * Generous, because the operator is meant to think about this. On expiry the
 * proposal is taken UNCHANGED — that is what would have happened without the
 * feature at all — and `pool_auto` records that no human confirmed it, so the
 * session's provenance stays honest. Aborting instead would throw away a
 * completed scoring phase because somebody made coffee. */
#define POOL_CONFIRM_TIMEOUT_MS  (15 * 60 * 1000)

/* Both pool sizes travel in and out by pointer. The euro count used to be
 * handed back through g_status.pool_n_euro, which is published at the TOP of
 * the wait loop and so still held the pre-confirmation size after a shrink:
 * unchecking a bonus number left the caller measuring comb(5,2)=10 draws over a
 * pool_euro[] whose tail was the previous proposal. A value the caller owns
 * cannot go stale that way. */
static void pool_confirm_wait(bool euro, int mx, int *io_pool_nm, int *io_pool_ne,
                              uint8_t *pool_main, uint8_t *pool_euro, int nm)
{
    g_status.pool_need_main = (uint8_t)nm;
    g_status.pool_need_euro = euro ? 2 : 0;
    g_status.pool_auto      = 0;
    int pool_nm = *io_pool_nm;
    int pool_ne = euro ? *io_pool_ne : 0;

    for (;;) {
        /* Publish the current proposal, then open the gate. Order matters: the
         * UI must never see PHASE_POOL_CONFIRM alongside a stale pool. */
        for (int i = 0; i < pool_nm; i++) g_status.pool_main[i] = pool_main[i];
        for (int i = 0; i < pool_ne; i++) g_status.pool_euro[i] = pool_euro[i];
        g_status.pool_n_main = (uint8_t)pool_nm;
        g_status.pool_n_euro = (uint8_t)pool_ne;
        s_pool_act     = POOL_WAIT;
        g_status.phase = PHASE_POOL_CONFIRM;

        int64_t deadline = esp_timer_get_time()
                         + (int64_t)POOL_CONFIRM_TIMEOUT_MS * 1000;
        PoolAction act;
        for (;;) {
            act = s_pool_act;
            if (act != POOL_WAIT) break;
            if (g_status.abort_requested) return;
            if (esp_timer_get_time() > deadline) {
                g_status.pool_auto = 1;
                s_pool_sel_nm = s_pool_sel_ne = 0;   // 0 = take it unchanged
                act = POOL_ACCEPT;
                printf("pool: no answer in %d min -- taking the scored proposal\n",
                       POOL_CONFIRM_TIMEOUT_MS / 60000);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            g_status.elapsed_ms = elapsed_ms_now();
        }

        if (act == POOL_CANCEL) { g_status.abort_requested = true; return; }

        if (act == POOL_ACCEPT) {
            if (s_pool_sel_nm > 0) {
                float z[POOL_MAIN_49];
                pool_scores_for(s_pool_sel_main, s_pool_sel_nm,
                                pool_main, g_status.pool_main_z, pool_nm, z);
                pool_nm = s_pool_sel_nm;
                for (int i = 0; i < pool_nm; i++) {
                    pool_main[i] = s_pool_sel_main[i];
                    g_status.pool_main_z[i] = z[i];
                }
            }
            if (euro && s_pool_sel_ne > 0) {
                float z[POOL_EURO_12];
                pool_scores_for(s_pool_sel_euro, s_pool_sel_ne,
                                pool_euro, g_status.pool_euro_z, pool_ne, z);
                pool_ne = s_pool_sel_ne;
                for (int i = 0; i < pool_ne; i++) {
                    pool_euro[i] = s_pool_sel_euro[i];
                    g_status.pool_euro_z[i] = z[i];
                }
            }
            /* Republish the CONFIRMED pool. The loop above only ever publishes
             * the proposal, so without this /status would keep describing a
             * pool that is not the one about to be measured. */
            for (int i = 0; i < pool_nm; i++) g_status.pool_main[i] = pool_main[i];
            for (int i = 0; i < pool_ne; i++) g_status.pool_euro[i] = pool_euro[i];
            g_status.pool_n_main = (uint8_t)pool_nm;
            g_status.pool_n_euro = (uint8_t)pool_ne;
            break;
        }

        /* POOL_MORE — measure again for the free slots only. The retained
         * numbers are omitted from the pass entirely, so they keep the score
         * that chose them instead of being re-measured. Each group refills
         * independently: unchecking a bonus number should not re-run the main
         * sweep, which is fifty runs of screen time. */
        g_status.phase = PHASE_SCORING;
        g_status.scoring_total = 0;      // fresh bar for the re-scoring pass
        g_status.scoring_done  = 0;
        if (s_pool_sel_nm < pool_nm) {
            float keep_z[POOL_MAIN_49];
            pool_scores_for(s_pool_sel_main, s_pool_sel_nm,
                            pool_main, g_status.pool_main_z, pool_nm, keep_z);
            score_and_build_pool(mx, pool_nm, pool_main, false,
                                 s_pool_sel_main, keep_z, s_pool_sel_nm,
                                 g_status.pool_main_z);
            if (g_status.abort_requested) return;
        }
        if (euro && s_pool_sel_ne < pool_ne) {
            float keep_z[POOL_EURO_12];
            pool_scores_for(s_pool_sel_euro, s_pool_sel_ne,
                            pool_euro, g_status.pool_euro_z, pool_ne, keep_z);
            score_and_build_pool(12, pool_ne, pool_euro, true,
                                 s_pool_sel_euro, keep_z, s_pool_sel_ne,
                                 g_status.pool_euro_z);
            if (g_status.abort_requested) return;
        }
        focus_off();
    }
    *io_pool_nm = pool_nm;
    *io_pool_ne = pool_ne;
}

static int cmp_desc(const void *a, const void *b)
{
    const RunResult *ra = (const RunResult *)a;
    const RunResult *rb = (const RunResult *)b;
    if (rb->z_score > ra->z_score) return  1;
    if (rb->z_score < ra->z_score) return -1;
    return 0;
}

static int cmp_asc(const void *a, const void *b)
{
    return -cmp_desc(a, b);
}


/* Combine this run's per-node z into one N(0,1) value: Σz / √k over the k nodes
 * that actually contributed. k varies run to run when a node is dropped, and
 * that is fine — each combined z is still unit-variance under the null, which
 * is the only property the statistics above depend on. Returns k via *n_used. */
static double combine_z(double z_master, int *n_used)
{
    double sum = 0.0;
    int    k   = 0;
    if (g_status.nodes[0].ok) { sum += z_master; k++; }
    for (int i = 0; i < nodes_slave_count(); i++) {
        double zs = 0.0;
        if (g_status.nodes[i + 1].ok && node_take_z(i, &zs)) { sum += zs; k++; }
    }
    if (n_used) *n_used = k;
    return k > 0 ? sum / sqrt((double)k) : 0.0;
}

/* One scoring run, node-combined like Phase 2: trigger every node, measure
 * locally in parallel, combine ÷√k. No baseline subtraction — the offset is
 * common to every number and scoring only ranks them. */
static double score_one_run(void)
{
    int nseg = segments_for();
    slave_trigger(nseg);
    double zm = 0.0;
    bool   ok = gcp_zscore_ok(nseg, &zm);
    // Sampling is over the moment the local run returns; the reply wait below
    // is dark time. The caller lit the panel — see focus_publish().
    focus_off();
    if (!ok) node_camera_failed(0, "stalled mid-run");
    if (nodes_have_slaves()) nodes_collect(LINK_MEAS_MS, true);
    return combine_z(zm, NULL);
}

/* Select the most-frequent numbers from the accumulated Z>2 histograms and
 * publish them (sorted ascending) to g_status.freq_*. */
static void publish_frequency(int *fm, int *fe, int z2, int nm, int mx, bool euro)
{
    g_status.freq_z2_count = z2;
    if (z2 <= 0) return;
    bool used[51] = {false};
    for (int k = 0; k < nm; k++) {
        int b = 0, bf = -1;
        for (int j = 1; j <= mx; j++)
            if (!used[j] && fm[j] > bf) { b = j; bf = fm[j]; }
        g_status.freq_nums[k] = (uint8_t)b;
        if (b) used[b] = true;
    }
    for (int i = 1; i < nm; i++) {
        uint8_t key = g_status.freq_nums[i]; int j = i - 1;
        while (j >= 0 && g_status.freq_nums[j] > key) { g_status.freq_nums[j+1] = g_status.freq_nums[j]; j--; }
        g_status.freq_nums[j+1] = key;
    }
    if (euro) {
        bool eu[13] = {false};
        for (int k = 0; k < 2; k++) {
            int b = 0, bf = -1;
            for (int j = 1; j <= 12; j++)
                if (!eu[j] && fe[j] > bf) { b = j; bf = fe[j]; }
            g_status.freq_euro[k] = (uint8_t)b;
            if (b) eu[b] = true;
        }
        if (g_status.freq_euro[0] > g_status.freq_euro[1]) {
            uint8_t t = g_status.freq_euro[0];
            g_status.freq_euro[0] = g_status.freq_euro[1];
            g_status.freq_euro[1] = t;
        }
    }
}

/* Bonferroni-corrected significance of the most extreme |Z| in the published
 * ranking — honest about the multiple-comparison search over `comparisons`. */
static void compute_significance(int comparisons)
{
    if (comparisons <= 0) {
        g_status.best_z = 0.0; g_status.p_corrected = 1.0; g_status.comparisons = 0;
        return;
    }
    double zt = (g_status.result_count > 0) ? fabs(g_status.top[0].z_score) : 0.0;
    double zb = (g_status.low_count   > 0) ? fabs(g_status.low[0].z_score) : 0.0;
    double zmax = zt > zb ? zt : zb;
    double p1 = erfc(zmax / 1.41421356237);   // two-sided single-test tail prob
    double pc = (double)comparisons * p1;      // Bonferroni
    if (pc > 1.0) pc = 1.0;
    g_status.best_z      = zmax;
    g_status.p_corrected = pc;
    g_status.comparisons = comparisons;
}

/* Studentize one loop's measurements: center on the loop's own mean and scale
 * by the loop's own empirical σ. This (a) removes the common bias offset with
 * a 5005-sample estimate instead of the noisy 100-run baseline (whose error
 * would otherwise accumulate √k-coherently in cumulative mode), and (b) makes
 * per-run Z exactly N(0,1) under the null even if raw source reads are
 * correlated and true σ ≠ 1. Reports the pre-scaling σ as a quality metric. */
static void studentize(int n, double *out_mean)
{
    if (out_mean) *out_mean = 0.0;
    if (n < 4) { g_status.loop_sigma = 0.0; return; }
    double m = 0.0;
    for (int i = 0; i < n; i++) m += g_status.results[i].z_score;
    m /= n;
    double v = 0.0;
    for (int i = 0; i < n; i++) {
        double d = g_status.results[i].z_score - m;
        v += d * d;
    }
    double s = sqrt(v / (n - 1));
    g_status.loop_sigma = s;
    if (out_mean) *out_mean = m;   // the offset removed here — Phase 3 drift check
    if (s < 1e-9) s = 1.0;
    for (int i = 0; i < n; i++) {
        double z = (g_status.results[i].z_score - m) / s;
        g_status.results[i].z_score = z;
        g_status.results[i].chi_sq  = z * z;
        g_status.results[i].p_value = p_label(fabs(z));
    }
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* After a mid-measurement abort the measured entries sit scattered at
 * results[s_perm[0..done-1]] (random order). Compact them into
 * results[0..done-1]: sorted ascending each source index is >= its
 * destination, so the stable forward copy never clobbers unread data. */
static void compact_partial(int done)
{
    qsort(s_perm, done, sizeof(uint16_t), cmp_u16);
    for (int j = 0; j < done; j++)
        g_status.results[j] = g_status.results[s_perm[j]];
}

/* Independence diagnostics over every pair of nodes — free bookkeeping that
 * verifies the √n combine assumption. At n=4 there are six pairs, i.e. six ways
 * for the assumption to be false, and ONE correlated pair invalidates the
 * combine. So the worst pair is what gets published; averaging the six would
 * let one bad pair hide behind five good ones.
 *
 * Moments are centered PER LOOP before folding into the session totals: pooling
 * raw values across loops would let each loop's random baseline offset
 * (SE = 1/√n_baseline per node) masquerade as correlation.
 *
 * A pair only accumulates on runs where BOTH nodes contributed, so a node that
 * was dropped part-way through does not silently shift the other's mean. */
typedef struct {
    double sx, sy, sxx, syy, sxy;   // this loop
    int    n;
    double cxx, cyy, cxy;           // session, per-loop centered
    int    cn, cloops;
} PairAcc;

typedef struct {
    double s, ss;                   // this loop
    int    n;
    double css;                     // session, per-loop centered
    int    cn, cloops;
} NodeAcc;

static PairAcc s_pair[MAX_NODES][MAX_NODES];   // upper triangle, i < j
static NodeAcc s_nacc[MAX_NODES];

static void pairs_reset(void)
{
    memset(s_pair, 0, sizeof(s_pair));
    memset(s_nacc, 0, sizeof(s_nacc));
}

/* Fold one run's per-node values in. `z[]` holds each node's own z and `have[]`
 * says which of them are real this run. */
static void pairs_add_run(const double *z, const bool *have)
{
    for (int i = 0; i < g_status.node_count; i++) {
        if (!have[i]) continue;
        s_nacc[i].s  += z[i];
        s_nacc[i].ss += z[i] * z[i];
        s_nacc[i].n++;
        for (int j = i + 1; j < g_status.node_count; j++) {
            if (!have[j]) continue;
            PairAcc *p = &s_pair[i][j];
            p->sx  += z[i];       p->sy  += z[j];
            p->sxx += z[i] * z[i]; p->syy += z[j] * z[j];
            p->sxy += z[i] * z[j];
            p->n++;
        }
    }
}

static void pairs_fold_loop(void)
{
    for (int i = 0; i < MAX_NODES; i++) {
        NodeAcc *a = &s_nacc[i];
        if (a->n >= 2) {
            double n = (double)a->n, m = a->s / n;
            a->css += a->ss - n * m * m;
            a->cn  += a->n;
            a->cloops++;
        }
        a->s = a->ss = 0.0; a->n = 0;

        for (int j = i + 1; j < MAX_NODES; j++) {
            PairAcc *p = &s_pair[i][j];
            if (p->n >= 2) {
                double n = (double)p->n, mx = p->sx / n, my = p->sy / n;
                p->cxx += p->sxx - n * mx * mx;
                p->cyy += p->syy - n * my * my;
                p->cxy += p->sxy - n * mx * my;
                p->cn  += p->n;
                p->cloops++;
            }
            p->sx = p->sy = p->sxx = p->syy = p->sxy = 0.0; p->n = 0;
        }
    }
}

static void publish_pair_stats(void)
{
    for (int i = 0; i < g_status.node_count; i++) {
        const NodeAcc *a = &s_nacc[i];
        int df = a->cn - a->cloops;          // one mean estimated per loop
        double v = (df >= 1) ? a->css / df : 0.0;
        g_status.nodes[i].sigma = v > 0.0 ? sqrt(v) : 0.0;
    }

    memset(g_status.pair_r, 0, sizeof(g_status.pair_r));
    double best = 0.0;
    int    bi = 0, bj = 0, bn = 0, count = 0;
    for (int i = 0; i < g_status.node_count; i++)
        for (int j = i + 1; j < g_status.node_count; j++) {
            const PairAcc *p = &s_pair[i][j];
            if (p->cn - p->cloops < 1 || p->cxx <= 0.0 || p->cyy <= 0.0) continue;
            count++;
            double r = p->cxy / sqrt(p->cxx * p->cyy);
            g_status.pair_r[i][j] = r;
            // |r| decides, but the signed value is what gets published — the
            // sign says whether nodes move together or against each other, and
            // that is the first clue to a mechanism if one ever shows up.
            if (fabs(r) >= fabs(best)) { best = r; bi = i; bj = j; bn = p->cn; }
        }
    g_status.pair_r_max = best;
    g_status.pair_r_i   = bi;
    g_status.pair_r_j   = bj;
    g_status.pair_n     = bn;
    g_status.pair_count = count;
}

/* ── Phase 3: cross-loop drift instrumentation ─────────────────────────
 * studentize() removes each loop's own mean exactly, so a *constant* offset
 * (e.g. the camera's residual LSB bias, ≈ −0.33 z/run in Phase 1) is harmless.
 * A trend across loops is not: it survives centering and, over a 20 h session,
 * has far more room to develop than the three loops of Phase 1/2. So regress
 * the master's raw per-run offset on the loop index and publish slope + t.
 * Running sums, so the test stays exact past LOOP_HIST stored loops. */
static struct { double n, sx, sxx, sy, sxy, syy; } s_drift;

/* Loops needed before the slope is published at all.
 *
 * SIX, not three. The regression has n−2 degrees of freedom, so at three loops
 * t has ONE — where the 5 % critical value is 12.7, not 2. The 10-loop session
 * of 2026-07-26 duly reported drift_t = +10.30 after loop 3 (p ≈ 0.06, i.e.
 * nothing) from two near-identical offsets and one outlier, then settled to
 * −0.20 by loop 10 as the df arrived. A flag that fires on noise is worse than
 * no flag, because the one time it matters nobody believes it. At six loops
 * df = 4 and the |t| > 3 threshold means roughly what it looks like. */
#define DRIFT_MIN_LOOPS 6

static void drift_add(double x, double y)
{
    s_drift.n++;
    s_drift.sx += x; s_drift.sxx += x * x;
    s_drift.sy += y; s_drift.sxy += x * y; s_drift.syy += y * y;
    if (s_drift.n < DRIFT_MIN_LOOPS) return;
    double n   = s_drift.n;
    double sxx = s_drift.sxx - s_drift.sx * s_drift.sx / n;
    double sxy = s_drift.sxy - s_drift.sx * s_drift.sy / n;
    double syy = s_drift.syy - s_drift.sy * s_drift.sy / n;
    if (sxx <= 0.0) return;
    double b     = sxy / sxx;
    double resid = syy - b * sxy;              // Σ of squared residuals
    if (resid < 0.0) resid = 0.0;
    double var_b = resid / (n - 2.0) / sxx;    // SE(slope)²
    g_status.drift_slope = b;
    g_status.drift_t     = (var_b > 0.0) ? b / sqrt(var_b) : 0.0;
}

/* Append one completed loop to the health table. Must run BEFORE
 * pairs_fold_loop(), which clears the per-loop sums this reads. */
static void record_loop(double loop_mean, int loop_idx)
{
    // Per-node mean and σ for this loop, straight from the un-folded sums.
    double mean_n[MAX_NODES] = {0}, sig_n[MAX_NODES] = {0};
    for (int i = 0; i < g_status.node_count; i++) {
        const NodeAcc *a = &s_nacc[i];
        if (a->n < 2) continue;
        double n = (double)a->n;
        mean_n[i] = a->s / n;
        double v = (a->ss - n * mean_n[i] * mean_n[i]) / (n - 1.0);
        sig_n[i] = v > 0.0 ? sqrt(v) : 0.0;
    }
    // Solo master: no per-node accumulation happened, so fall back to the
    // combined mean, which is the master's own mean in that case.
    if (s_nacc[0].n < 2) mean_n[0] = loop_mean;

    camera_stats_t cs;
    camera_get_stats(&cs);
    slaves_diag();                    // nodes are idle between loops

    // Close out this loop's window/gap means before the next loop reopens them.
    float win_ms = 0.0f, gap_ms = 0.0f;
    focus_timing_take(&win_ms, &gap_ms);

    if (g_status.loop_hist && g_status.loop_hist_n < LOOP_HIST) {
        LoopStat *L = &g_status.loop_hist[g_status.loop_hist_n++];
        memset(L, 0, sizeof(*L));
        L->base  = (float)g_status.baseline_mean;
        L->mean  = (float)loop_mean;
        L->sigma = (float)g_status.loop_sigma;
        L->nodes = (uint8_t)g_status.node_count;
        L->t_s   = (uint32_t)(g_status.elapsed_ms / 1000);
        L->cal_ms = (uint16_t)(g_status.cal_ms > 65535 ? 65535 : g_status.cal_ms);
        L->win_ms = win_ms;
        L->gap_ms = gap_ms;
        for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
            L->mean_n[i] = (float)mean_n[i];
            L->sig_n[i]  = (float)sig_n[i];
            L->cam_mbit[i]   = i ? g_status.nodes[i].cam_mbit : (float)cs.mbit_per_sec;
            L->cam_stalls[i] = i ? g_status.nodes[i].cam_stalls : cs.stalls;
            // The operating point this loop was measured AT (§1.5.2). Per-loop
            // re-tuning is only safe because it is recorded: without this the
            // setting change and a drift in the data look the same afterwards.
            L->cam_exp[i]    = g_status.nodes[i].cam_exp;
            L->cam_gain[i]   = g_status.nodes[i].cam_gain;
            L->cam_fold[i]   = g_status.nodes[i].cam_fold;
            L->cam_cal_ok[i] = g_status.nodes[i].cam_cal_ok;
            L->cam_bias[i]   = g_status.nodes[i].cam_bias;
        }
    }
    g_status.nodes[0].cam_mbit   = (float)cs.mbit_per_sec;
    g_status.nodes[0].cam_stalls = cs.stalls;

    double s = g_status.loop_sigma;
    if (s > 0.0) {
        if (g_status.loops_done == 0 || s < g_status.sigma_lo) g_status.sigma_lo = s;
        if (g_status.loops_done == 0 || s > g_status.sigma_hi) g_status.sigma_hi = s;
    }
    // The master's RAW per-run offset: what its source actually produced this
    // loop, before studentization removed it (baseline + post-baseline residual)
    double raw_off = g_status.baseline_mean + mean_n[0];
    if (g_status.loops_done == 0) g_status.off_first = raw_off;
    g_status.off_last = raw_off;
    g_status.loops_done++;

    drift_add((double)loop_idx, raw_off);
}

/* Greedy diversified "coverage" picks: from the COVER_POOL most extreme
 * combinations (highest Z if !lowest, most-negative if lowest), choose up to
 * TOP_N that each share at most nm/2 numbers with every already-chosen one —
 * strong by Z but spread out, so the set collectively covers more of the draw
 * space than the (often near-duplicate) raw top-N / bottom-N. Operates on
 * g_status.results[] in combination-index order (cumulative mode). */
#define COVER_POOL 48
static void publish_coverage(int n, int nm, bool lowest,
                             RunResult *out, int *out_count)
{
    if (n <= 0) { *out_count = 0; return; }

    // Gather the COVER_POOL most extreme combinations by Z (best candidate first)
    int candIdx[COVER_POOL];
    int mc = 0;
    for (int i = 0; i < n; i++) {
        double z = g_status.results[i].z_score;
        double worst = (mc > 0) ? g_status.results[candIdx[mc - 1]].z_score : 0.0;
        bool better = (mc < COVER_POOL) || (lowest ? (z < worst) : (z > worst));
        if (!better) continue;
        if (mc < COVER_POOL) mc++;
        int p = mc - 1;
        while (p > 0 && (lowest ? (z < g_status.results[candIdx[p - 1]].z_score)
                                : (z > g_status.results[candIdx[p - 1]].z_score))) {
            candIdx[p] = candIdx[p - 1]; p--;
        }
        candIdx[p] = i;
    }

    int  maxov = nm / 2;
    bool chosen[COVER_POOL] = {false};
    int  sel = 0;
    // Pass 1: greedy by Z, enforce the pairwise overlap constraint
    for (int j = 0; j < mc && sel < TOP_N; j++) {
        RunResult *cj = &g_status.results[candIdx[j]];
        bool ok = true;
        for (int s = 0; s < sel && ok; s++) {
            int shared = 0;
            for (int a = 0; a < nm; a++)
                for (int b = 0; b < nm; b++)
                    if (out[s].nums[a] == cj->nums[b]) shared++;
            if (shared > maxov) ok = false;
        }
        if (ok) { out[sel++] = *cj; chosen[j] = true; }
    }
    // Pass 2: if the constraint was too tight, fill remaining slots by Z
    for (int j = 0; j < mc && sel < TOP_N; j++) {
        if (!chosen[j]) out[sel++] = g_status.results[candIdx[j]];
    }
    *out_count = sel;
}

/* Cumulative (Stouffer) ranking: each of the n fixed combinations has its
 * running Σz in zsum[] over k measured loops. Rank by Z = Σz/√k, publish the
 * top-N / bottom-N and most-frequent (over cumulative Z>2). */
static void publish_cumulative(double *zsum, int n, int k,
                               int *fm, int *fe, int *z2, int nm, int mx, bool euro)
{
    if (k <= 0 || n <= 0) return;
    double sk = sqrt((double)k);
    for (int i = 0; i < n; i++) {
        double z = zsum[i] / sk;                  // Stouffer Z = Σz / √k
        g_status.results[i].z_score = z;
        g_status.results[i].chi_sq  = z * z;
        g_status.results[i].p_value = p_label(fabs(z));
    }

    // Select top-N (highest) and bottom-N (lowest) by insertion, WITHOUT
    // sorting results[] — it must stay in combination-index order so the next
    // loop's Σz accumulation and nums stay aligned by index.
    RunResult top[TOP_N], low[TOP_N];
    int tn = 0, ln = 0;
    for (int i = 0; i < n; i++) {
        RunResult *r = &g_status.results[i];
        if (tn < TOP_N || r->z_score > top[tn ? tn - 1 : 0].z_score) {
            if (tn < TOP_N) tn++;
            int p = tn - 1;
            while (p > 0 && top[p - 1].z_score < r->z_score) { top[p] = top[p - 1]; p--; }
            top[p] = *r;
        }
        if (ln < TOP_N || r->z_score < low[ln ? ln - 1 : 0].z_score) {
            if (ln < TOP_N) ln++;
            int p = ln - 1;
            while (p > 0 && low[p - 1].z_score > r->z_score) { low[p] = low[p - 1]; p--; }
            low[p] = *r;
        }
    }
    for (int i = 0; i < tn; i++) g_status.top[i] = top[i];
    g_status.result_count = tn;
    for (int i = 0; i < ln; i++) g_status.low[i] = low[i];
    g_status.low_count = ln;

    // Most-frequent over cumulative Z>2 (scan, order-independent)
    for (int j = 0; j <= 50; j++) fm[j] = 0;
    for (int j = 0; j <= 12; j++) fe[j] = 0;
    *z2 = 0;
    for (int i = 0; i < n; i++) {
        if (g_status.results[i].z_score <= 2.0) continue;
        (*z2)++;
        for (int j = 0; j < nm; j++) fm[g_status.results[i].nums[j]]++;
        if (euro) { fe[g_status.results[i].euro[0]]++; fe[g_status.results[i].euro[1]]++; }
    }
    publish_frequency(fm, fe, *z2, nm, mx, euro);

    // Diversified max-spread picks from the top-Z and bottom-Z pools
    publish_coverage(n, nm, false, g_status.cover,     &g_status.cover_count);
    publish_coverage(n, nm, true,  g_status.cover_low, &g_status.cover_low_count);
}

/* Fold one completed (or partial) loop's results into the cumulative top-N
 * carry, accumulate the cross-loop Z>2 frequency histograms, and publish the
 * current cumulative top-N + most-frequent so /status can show them between
 * loops (intermediate results), not only at the very end. */
static void absorb_loop(RunResult *carry, int *carry_n,
                        RunResult *low, int *low_n,
                        int *fm, int *fe, int *z2, int nm, int mx, bool euro)
{
    int done = g_status.runs_completed;
    if (done > 0) {
        qsort(g_status.results, done, sizeof(RunResult), cmp_desc);

        // Frequency accumulation over Z>2 runs (sorted desc → stop at <=2)
        for (int i = 0; i < done; i++) {
            if (g_status.results[i].z_score <= 2.0) break;
            (*z2)++;
            for (int j = 0; j < nm; j++) fm[g_status.results[i].nums[j]]++;
            if (euro) { fe[g_status.results[i].euro[0]]++; fe[g_status.results[i].euro[1]]++; }
        }

        int take = done < TOP_N ? done : TOP_N;
        RunResult tmp[2 * TOP_N];
        int tn;

        // Merge this loop's highest-Z into the top carry, keep global top-N
        tn = 0;
        for (int i = 0; i < *carry_n; i++) tmp[tn++] = carry[i];
        for (int i = 0; i < take; i++) tmp[tn++] = g_status.results[i];   // results[] sorted desc
        qsort(tmp, tn, sizeof(RunResult), cmp_desc);
        *carry_n = tn < TOP_N ? tn : TOP_N;
        for (int i = 0; i < *carry_n; i++) carry[i] = tmp[i];

        // Merge this loop's lowest-Z into the low carry, keep global bottom-N
        tn = 0;
        for (int i = 0; i < *low_n; i++) tmp[tn++] = low[i];
        for (int i = 0; i < take; i++) tmp[tn++] = g_status.results[done - take + i];
        qsort(tmp, tn, sizeof(RunResult), cmp_asc);
        *low_n = tn < TOP_N ? tn : TOP_N;
        for (int i = 0; i < *low_n; i++) low[i] = tmp[i];
    }

    // Publish cumulative top-N + bottom-N (entries first, then count)
    for (int i = 0; i < *carry_n; i++) g_status.top[i] = carry[i];
    g_status.result_count = *carry_n;
    for (int i = 0; i < *low_n; i++) g_status.low[i] = low[i];
    g_status.low_count = *low_n;
    g_status.cover_count = g_status.cover_low_count = 0;   // coverage is cumulative-only

    // Publish most-frequent numbers across all loops' Z>2 runs
    publish_frequency(fm, fe, *z2, nm, mx, euro);
}

void elotto_task(void *pvParam)
{
    g_status.state           = ELOTTO_RUNNING;
    g_status.runs_completed  = 0;
    g_status.baseline_done   = 0;
    g_status.scoring_done    = 0;
    g_status.abort_requested = false;
    g_status.elapsed_ms      = 0;
    g_status.baseline_mean   = 0.0;
    g_status.slave_connected = false;
    g_status.result_count    = 0;
    g_status.low_count       = 0;
    g_status.cover_count     = 0;
    g_status.cover_low_count = 0;
    g_status.freq_z2_count   = 0;
    g_status.loop_current    = 0;
    g_status.best_z          = 0.0;
    g_status.p_corrected     = 1.0;
    g_status.comparisons     = 0;
    g_status.loop_sigma      = 0.0;
    g_status.loops_done      = 0;
    g_status.loop_hist_n     = 0;
    g_status.drift_slope     = 0.0;
    g_status.drift_t         = 0.0;
    g_status.off_first       = 0.0;
    g_status.off_last        = 0.0;
    g_status.sigma_lo        = 0.0;
    g_status.sigma_hi        = 0.0;
    g_status.cal_ms          = 0;
    memset(&s_drift, 0, sizeof(s_drift));
    // focus_mode is set by /start and NOT reset here — it is the session's tag.
    // Everything else about the panel starts clean, including a pause left over
    // from a session that was aborted while held.
    focus_reset();
    // Loop history lives in PSRAM: results[] already fills internal RAM. Kept
    // for the lifetime of the app (allocated once, never freed) so the table of
    // a finished session survives for inspection.
    if (!g_status.loop_hist)
        g_status.loop_hist = heap_caps_calloc(LOOP_HIST, sizeof(LoopStat), MALLOC_CAP_SPIRAM);
    g_status.pair_r_max      = 0.0;
    g_status.pair_n          = 0;
    g_status.pair_count      = 0;
    g_status.pair_r_i        = g_status.pair_r_j = 0;
    // Per-session transport health. "Zero lost triggers" is the Phase C gate,
    // so it has to be a counted number rather than an impression.
    g_status.net_retries     = 0;
    g_status.net_lost        = 0;
    g_status.net_stale       = 0;
    if (g_status.baseline_total <= 0 || g_status.baseline_total > 5000)
        g_status.baseline_total = 100;
    if (g_status.loops_total <= 0 || g_status.loops_total > 500)
        g_status.loops_total = 1;

    // Re-discover every session: a node that was powered off last time joins
    // this one, and one that vanished does not sit in the table as a phantom.
    nodes_discover();

    // After abort_requested is cleared, so a camera that is not ready can abort
    // the session immediately instead of having the flag wiped by the reset above.
    camera_source_begin();

    bool euro    = (g_status.mode == MODE_EUROJACKPOT);
    int  nm      = euro ? 5 : 6;
    int  mx      = euro ? 50 : 49;
    int  pool_nm = euro ? POOL_MAIN_50 : POOL_MAIN_49;

    g_status.scoring_total = mx + (euro ? 12 : 0);   // one run per number

    uint8_t pool_main[POOL_MAIN_49] = {0};   // 15 slots, enough for both modes
    uint8_t pool_euro[POOL_EURO_12] = {0};
    // Both pool sizes are variables, not constants: attended confirmation can
    // shrink either, and every count downstream is derived from them.
    int     pool_ne     = euro ? POOL_EURO_12 : 0;
    int     main_combos = comb(pool_nm, nm);
    int     euro_combos = euro ? comb(pool_ne, 2) : 1;
    int     full_combos = main_combos * euro_combos;
    g_status.runs_total = (g_status.runs_limit > 0 && g_status.runs_limit < full_combos)
                          ? g_status.runs_limit : full_combos;

    // Peak-mode carries (best/worst single-run Z across loops) + freq histograms
    RunResult carry[TOP_N];
    RunResult low_carry[TOP_N];
    int carry_n = 0, low_n = 0;
    int fm[51] = {0}, fe[13] = {0}, z2 = 0;

    // Cumulative (Stouffer) mode: fixed pool, Σz per combination over meas_k loops
    bool cumulative = (g_status.rank_mode == RANK_CUMULATIVE);
    int  meas_k = 0;
    if (cumulative)
        memset(s_zsum, 0, sizeof(double) * (size_t)g_status.runs_total);

    // Pairwise independence check across all nodes (per-loop centered)
    pairs_reset();

    int comparisons = 0;
    s_t0 = esp_timer_get_time();

    for (int loop = 0; loop < g_status.loops_total; loop++) {
        g_status.loop_current   = loop + 1;
        g_status.baseline_done  = 0;
        g_status.scoring_done   = 0;
        g_status.runs_completed = 0;
        g_status.baseline_mean  = 0.0;
        // Cumulative mode locks the pool after loop 0; peak mode rebuilds it
        bool do_score = (!cumulative || loop == 0);
        if (do_score) {
            memset(pool_main, 0, sizeof(pool_main));
            memset(pool_euro, 0, sizeof(pool_euro));
        }

        /* ── Task 1: camera calibration (every node in parallel) ──────── */
        // Before the baseline, because the baseline estimates the offset of the
        // runs it will be subtracted from and must therefore be measured on the
        // SAME operating point those runs use. Calibrating after it would leave
        // the whole loop correcting for an offset from a setting it no longer
        // runs on. A full sweep every loop is the decision in §1.5.1: the
        // optimum is not assumed constant, and re-deriving it is what tracks
        // thermal drift.
        calibrate_all();
        if (g_status.abort_requested) { slave_abort(); goto done; }

        /* ── Phase 1: Baseline calibration (every node in parallel) ────── */
        // Baseline runs at MEASUREMENT length: it estimates the offset of the
        // runs it will be subtracted from, so it has to be the same instrument.
        // Nothing is displayed — the panel is hidden during baseline — and
        // pause is deliberately not offered here: one 'B' command sets every
        // slave running its whole baseline autonomously, so a master-side hold
        // would desynchronise them rather than pause them.
        g_status.phase = PHASE_BASELINE;
        slave_baseline_start(g_status.baseline_total, segments_for());
        {
            double bsum = 0.0;
            int    bn   = 0;
            for (int i = 0; i < g_status.baseline_total; i++) {
                if (g_status.abort_requested) {
                    slave_abort();
                    goto done;
                }
                double bz = 0.0;
                if (gcp_zscore_ok(segments_for(), &bz)) { bsum += bz; bn++; }
                else {
                    // A void run must not be averaged in as a zero — that would
                    // pull the offset toward 0 and quietly bias every run it is
                    // later subtracted from.
                    node_camera_failed(0, "stalled during baseline");
                    break;
                }
                run_gap();          // same duty cycle as the runs it calibrates
                g_status.baseline_done = i + 1;
                g_status.elapsed_ms = elapsed_ms_now();
            }
            g_status.baseline_mean = bn ? bsum / bn : 0.0;
        }
        if (nodes_have_slaves() && !g_status.abort_requested)
            slave_baseline_wait();

        /* ── Phase 0: Individual number scoring (cumulative: loop 0 only) ── */
        g_status.phase = PHASE_SCORING;
        if (do_score) {
            // Zero the pair for this loop's group of passes; each pass adds its
            // own measured count, so main and bonus share one bar.
            g_status.scoring_total = 0;
            g_status.scoring_done  = 0;
            score_and_build_pool(mx, pool_nm, pool_main, false,
                                 NULL, NULL, 0, g_status.pool_main_z);
            if (g_status.abort_requested) goto done;
            if (euro) score_and_build_pool(12, POOL_EURO_12, pool_euro, true,
                                           NULL, NULL, 0, g_status.pool_euro_z);
            if (g_status.abort_requested) goto done;
            focus_off();

            /* ── Attended pool confirmation ────────────────────────────
             * Stop and show the operator what scoring chose, before Phase 2
             * spends thousands of runs on it. Only when the session asked for
             * it (`?confirm=1`): a device that blocks on a human would hang
             * every scripted run, and the ?cal=0 control is started by curl.
             *
             * Loop 0 only, and that falls out of `do_score` — in cumulative
             * mode the pool is locked after loop 0, so there is nothing left
             * to confirm afterwards. */
            if (g_status.pool_confirm) {
                pool_confirm_wait(euro, mx, &pool_nm, &pool_ne,
                                  pool_main, pool_euro, nm);
                if (g_status.abort_requested) goto done;
                /* The operator may have removed numbers, so the combination
                 * space is not the constant it was at session start. Recompute
                 * everything derived from the pool sizes — at the minimum
                 * (pool == draw size) this is exactly ONE combination.
                 * pool_nm/pool_ne were updated in place above. */
                main_combos = comb(pool_nm, nm);
                euro_combos = euro ? comb(pool_ne, 2) : 1;
                full_combos = main_combos * euro_combos;
                g_status.runs_total =
                    (g_status.runs_limit > 0 && g_status.runs_limit < full_combos)
                    ? g_status.runs_limit : full_combos;
                if (g_status.runs_total > NUM_RUNS) g_status.runs_total = NUM_RUNS;
                // The Stouffer accumulator was sized for the pre-confirmation
                // combination count; the pool may now be smaller, so clear it
                // to the new length before loop 0 starts writing into it.
                memset(s_zsum, 0, sizeof(double) * (size_t)g_status.runs_total);
            }
        } else {
            g_status.scoring_done = g_status.scoring_total;   // pool already locked
        }

        /* ── Phase 2: Measure all combinations in fresh random order ───── */
        // Fisher–Yates: with a fixed order, slow drift (temperature ramp over
        // the ~20-min loop) would hit each combination at the same position
        // every loop and accumulate √k-coherently — exactly like a real signal.
        for (int i = 0; i < g_status.runs_total; i++) s_perm[i] = (uint16_t)i;
        for (int i = g_status.runs_total - 1; i > 0; i--) {
            // Deliberately fast_rng(), not the camera: this is administrative
            // randomness (measurement order), not measured data. Spending
            // rate-limited camera entropy here would stall the session for
            // bits that never enter a z-score.
            int j = (int)(fast_rng() % (uint32_t)(i + 1));
            uint16_t t = s_perm[i]; s_perm[i] = s_perm[j]; s_perm[j] = t;
        }

        g_status.phase = PHASE_MEASURING;
        for (int j = 0; j < g_status.runs_total; j++) {
            if (g_status.abort_requested) {
                slave_abort();
                goto done;
            }
            // Between runs, never inside one: the run just finished is kept and
            // j does not advance, so nothing is lost or duplicated.
            pause_gate();
            if (g_status.abort_requested) {
                slave_abort();
                goto done;
            }
            int i = s_perm[j];   // slot index; results[] stays slot-indexed

            // Slot → combination: spread slots evenly over the full space so a
            // Runs cap samples across all combinations instead of taking the
            // lexicographic prefix (which all shares the pool's lowest numbers).
            // Uncapped, runs_total == full_combos and c == i exactly.
            int c  = (int)(((int64_t)i * full_combos) / g_status.runs_total);
            int mi = c % main_combos;
            int ei = euro ? (c / main_combos) : 0;
            nth_combination(pool_main, pool_nm, nm, mi, g_status.results[i].nums);
            if (euro)
                nth_combination(pool_euro, pool_ne, 2, ei, g_status.results[i].euro);
            else
                g_status.results[i].euro[0] = g_status.results[i].euro[1] = 0;

            // The draw goes on screen BEFORE the trigger, so the observer is
            // already attending when the first bit is sampled.
            focus_publish(FOCUS_DRAW, g_status.results[i].nums, nm,
                          g_status.results[i].euro, euro ? 2 : 0);

            // One broadcast starts every node, then measure locally — all of
            // them integrate the same window, which is the premise the √n
            // combine rests on.
            int nseg = segments_for();
            slave_trigger(nseg);
            double zraw = 0.0;
            bool   zok  = gcp_zscore_ok(nseg, &zraw);
            double zm   = zraw - g_status.baseline_mean;
            focus_off();          // sampling done; the reply wait is dark time
            if (!zok) node_camera_failed(0, "stalled mid-run");
            if (nodes_have_slaves()) nodes_collect(LINK_MEAS_MS, true);

            // Per-node values for the independence check, gathered before the
            // combine so a dropped node is excluded from both consistently.
            double znode[MAX_NODES] = {0};
            bool   have[MAX_NODES]  = {false};
            double sum = 0.0;
            int    k   = 0;
            if (zok && g_status.nodes[0].ok) {
                znode[0] = zm; have[0] = true; sum += zm; k++;
            }
            for (int s = 0; s < nodes_slave_count(); s++) {
                double zs = 0.0;
                if (!g_status.nodes[s + 1].ok || !node_take_z(s, &zs)) continue;
                znode[s + 1] = zs; have[s + 1] = true;
                sum += zs; k++;
            }
            pairs_add_run(znode, have);
            double z = (k > 0) ? sum / sqrt((double)k) : 0.0;

            g_status.results[i].index   = i + 1;
            g_status.results[i].z_score = z;
            g_status.results[i].chi_sq  = z * z;
            g_status.results[i].p_value = p_label(fabs(z));
            g_status.runs_completed     = j + 1;
            g_status.elapsed_ms         = elapsed_ms_now();
            run_gap();
        }

        // Loop finished cleanly: studentize on the loop's own mean/σ, then
        // fold + publish the ranking
        double loop_mean = 0.0;
        studentize(g_status.runs_total, &loop_mean);
        record_loop(loop_mean, loop);   // before the fold clears the per-loop sums
        pairs_fold_loop();
        publish_pair_stats();
        if (cumulative) {
            for (int i = 0; i < g_status.runs_total; i++)
                s_zsum[i] += g_status.results[i].z_score;   // Σz per fixed combination
            meas_k++;
            publish_cumulative(s_zsum, g_status.runs_total, meas_k, fm, fe, &z2, nm, mx, euro);
            comparisons = g_status.runs_total;
        } else {
            absorb_loop(carry, &carry_n, low_carry, &low_n, fm, fe, &z2, nm, mx, euro);
            comparisons = g_status.runs_total * g_status.loop_current;
        }
        compute_significance(comparisons);
    }
    goto finalize;

done:
    // Aborted mid-loop: publish whatever was completed so far. Measured
    // entries of the aborted loop sit scattered at results[s_perm[...]], so
    // compact them to the front before using them as a partial prefix.
    pairs_fold_loop();
    publish_pair_stats();
    if (cumulative) {
        if (meas_k > 0) {
            // Complete loops exist: discard the partial loop, publish those
            publish_cumulative(s_zsum, g_status.runs_total, meas_k, fm, fe, &z2, nm, mx, euro);
            comparisons = g_status.runs_total;
        } else if (g_status.runs_completed > 0) {
            // Aborted during the first measurement loop: treat the measured
            // subset as a single sample so partial results are still shown
            int pdone = g_status.runs_completed;
            compact_partial(pdone);
            studentize(pdone, NULL);
            for (int i = 0; i < pdone; i++)
                s_zsum[i] = g_status.results[i].z_score;
            publish_cumulative(s_zsum, pdone, 1, fm, fe, &z2, nm, mx, euro);
            comparisons = pdone;
        }
    } else {
        int pdone = g_status.runs_completed;
        if (pdone > 0 && pdone < g_status.runs_total) {
            compact_partial(pdone);
            studentize(pdone, NULL);
        }
        absorb_loop(carry, &carry_n, low_carry, &low_n, fm, fe, &z2, nm, mx, euro);
        comparisons = g_status.runs_total * (g_status.loop_current > 0 ? g_status.loop_current : 1);
    }
    compute_significance(comparisons);

finalize:
    focus_off();
    g_status.paused     = false;
    g_status.elapsed_ms = elapsed_ms_now();
    g_status.state = g_status.abort_requested ? ELOTTO_ABORTED : ELOTTO_DONE;
    vTaskDelete(NULL);
}
