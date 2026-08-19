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
#include "focus.h"
#include "elotto_link.h"

ElottoStatus g_status = { .state = ELOTTO_IDLE };

/* v3.0 (PLAN.md §2): one pass, every combination exactly once. The cross-loop
 * accumulators (Σz, high/low water marks) are gone with the loops themselves —
 * results[] IS the whole record, raw, in measurement order.
 *
 * Running sums instead, two scopes:
 *  - the BLOCK (between two sweep+baseline insertions) feeds the per-block σ,
 *    the /loops row and the drift regression;
 * No cross-item accumulator changes a measured value. */
static double s_blk_sum, s_blk_sumsq;
static int    s_blk_n;

// Random measurement order for the pass (Fisher–Yates, once per session) —
// decouples slow source drift from the combination enumeration, so drift
// spreads over random combinations instead of the lexicographic tail
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
 * ⚠ EVERY NUMBER IN THE TABLE ABOVE IS PRE-2026-08-18, i.e. the ~3,4 Mbit/s
 * instrument. Extraction is now 1,67x faster (5,71 Mbit/s idle, ~3,8 under
 * load), so the segment counts these runs solved for no longer describe this
 * rig — RUN_SEGS_REF/RUN_MS_REF in sensor.h carry the current calibration. The
 * SHAPE of the argument survives and the mechanism is now known: the cliff is
 * the GCP consumer preempting the capture task, not the duty cycle as such.
 * Re-measure before quoting any of these figures.
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

/* Segment count for a requested wall window. Uses the 2026-08-02 live cal
 * (66000 segs ↔ 4680 ms). Longer requests may stretch past the target: the
 * camera rate falls under sustained load (duty-cycle cliff) — that stretch IS
 * the limit the operator is probing with the ?run= field. */
static int segs_from_run_ms(int run_ms)
{
    if (run_ms < 100) run_ms = 100;
    long long n = ((long long)run_ms * RUN_SEGS_REF + RUN_MS_REF / 2) / RUN_MS_REF;
    if (n < 500) n = 500;
    if (n > EL_SEG_MAX) n = EL_SEG_MAX;   /* the wire's limit, elotto_link.h */
    return (int)n;
}

// Called once per session, after discovery, before any measurement.
static void camera_source_begin(void)
{
    g_status.noise_stalled = false;
    g_status.fault[0]      = '\0';
    prng_seed();
    if (!camera_is_ready())
        node_camera_failed(0, "not streaming at session start");
}

/* Segments for one run. v3: ONE count for every phase — baseline, scoring and
 * the measurement pass all use g_status.run_segments behind g_status.gap_ms.
 * The count still travels on the wire (`M<seg>`, `B<runs>,<seg>`) so a slave
 * cannot disagree about it. Baseline uses the same length for the
 * same-instrument rule. Defaults are filled in on /start if unset. */
static int segments_for(void)
{
    if (g_status.run_segments <= 0)
        g_status.run_segments = segs_from_run_ms(
            g_status.run_target_ms > 0 ? g_status.run_target_ms
                                       : RUN_S_DEFAULT * 1000);
    return g_status.run_segments;
}

static int gap_for(void)
{
    if (g_status.gap_ms <= 0) return SCORE_GAP_MS;
    return g_status.gap_ms;
}

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

/* ── Unlimited mode: how big a pool fits `cap` measurement runs ────────
 *
 * **Maximise the combinations measured**, and nothing else. The reason is in
 * sensor.h and is worth repeating in one line: the chance that a round's pool
 * contains the real draw is (combinations measured)/(combinations that exist) —
 * the main/bonus split cancels out of it entirely — so leaving runs unspent is
 * the ONLY way a pool rule can hurt, and it hurts twice over, because a short
 * round also pays a full 62-run scoring pass sooner.
 *
 * 6-of-49 falls out of the same objective: with no second pool, more
 * combinations is more numbers (cap 100 -> 9 numbers, 84 combinations).
 *
 * Eurojackpot's C(p,5)·C(q,2) <= cap does have several splits reaching the same
 * maximum, and there P is identical, so the choice is free — that is where the
 * user's preference for bonus numbers lives (2026-08-18): **ties go to the
 * larger bonus pool**, then to the larger main pool. It costs no coverage,
 * which is the whole reason it is a tie-break and not the objective.
 *
 * The minimum (p = draw size, q = 2) is one combination, so any cap >= 1 has a
 * solution. */
static void unlimited_pool_sizes(bool euro, int nm, int cap,
                                 int *out_nm, int *out_ne)
{
    int p_max = euro ? POOL_MAIN_50 : POOL_MAIN_49;
    int q_min = euro ? 2 : 0;
    int q_max = euro ? POOL_EURO_12 : 0;
    int  best_p = nm, best_q = q_min;
    long best_c = 0;

    for (int p = nm; p <= p_max; p++) {
        long cm = comb(p, nm);
        for (int q = q_min; q <= q_max; q++) {
            long total = cm * (euro ? comb(q, 2) : 1);
            if (total > cap) continue;
            /* Strictly more combinations wins; an equal count goes to the
             * bigger bonus pool, then the bigger main pool. */
            if (total > best_c ||
                (total == best_c && (q > best_q ||
                                     (q == best_q && p > best_p)))) {
                best_c = total;
                best_p = p;
                best_q = q;
            }
        }
    }
    *out_nm = best_p;
    if (out_ne) *out_ne = best_q;
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
// Phase 2), sweeping the numbers in random order, then picks pool_size numbers
// by the pre-registered score_dir (high / low / |z|). The pool is locked for
// the whole pass, so this is where selection confidence matters most.
//
// ⚠ Under H₀ the combined z is already N(0,1): its SE is 1.0, not 1/√k. The
// old "SE = 0.50 at four nodes" reading confused the per-node mean with the
// Stouffer statistic actually stored. Ranking 50/62 unit-noise draws is
// therefore very noisy — a real cost, stated once. It changes only WHICH
// numbers enter the pool, never the Phase-2 statistics measured on them.
// A session whose pool choice must be trusted on its own wants several full
// random passes; it must NOT go back to repeats in place (onset is the payload).
static double score_one_run(void);   // forward (defined after the slave link block)

/* Per-item node-z archive (PSRAM). results[j] stays lean; re-analysis needs
 * the per-node series to recombine, drop a soft-failed node, or recompute r. */
static float *s_node_z;   // [NUM_RUNS * MAX_NODES], NaN = did not contribute

static void node_z_store(int j, const double *znode, const bool *have)
{
    if (!s_node_z || j < 0 || j >= NUM_RUNS) return;
    float *row = s_node_z + (size_t)j * MAX_NODES;
    for (int i = 0; i < MAX_NODES; i++)
        row[i] = (have && have[i]) ? (float)znode[i] : NAN;
}

bool results_node_z(int j, float out[MAX_NODES])
{
    if (!out || !s_node_z || j < 0 || j >= g_status.runs_completed || j >= NUM_RUNS)
        return false;
    const float *row = s_node_z + (size_t)j * MAX_NODES;
    for (int i = 0; i < MAX_NODES; i++) out[i] = row[i];
    return true;
}

/* Running pass sums over RANKED items only (k > 0, !skip_rank). Ranking and
 * Bonferroni recompute from results[] after every valid item so studentized
 * Top/Bottom track the live mean/σ rather than an early snapshot. */
static double s_pass_sum, s_pass_sumsq;
static int    s_pass_n;

/* Which nodes actually entered the combine during the block center_block() last
 * looked at. Set there because it already walks that block's rows, and read by
 * record_loop() immediately afterwards to decide whether a trip could have
 * contaminated anything. */
static uint8_t s_blk_contrib;

/* Consecutive clean blocks while a node is soft_down (sticky clear). */
static uint8_t s_soft_clean[MAX_NODES];

/* Brief ring flush before an attended measurement window so pre-onset bits
 * do not enter the run the observer is about to watch. Master-only: slaves
 * have no settle command on the wire; calibration already settles every node. */
/* Every measurement window starts on fresh bits — ATTENDED OR NOT.
 *
 * Two things were wrong here until 2026-08-19.
 *
 * 1. `if (!g_status.focus_mode) return;` made the settle a property of the
 *    attended mode. But ?focus= is supposed to change NOTHING except the panel
 *    and the tag — "a matched no-focus control must be identical in every other
 *    respect" — and this made the two modes differ in what bits reach a run.
 *    The attended-vs-unattended comparison would have measured the settle as
 *    much as the observer.
 *
 * 2. It called camera_stats_reset(1), which also zeroes the camera statistics.
 *    Per item, that left every block's cam_bias / cam_mbit in /loops describing
 *    only that block's LAST item — in attended sessions, all of them. It now
 *    flushes the ring and leaves the accumulators alone.
 *
 * Why it is needed at all, in either mode: nothing consumes the ring during the
 * gap, so it is FULL when a window opens — 524288 bits produced before the item
 * existed, which is 10 % of a run=1 item. Those bits are not bad, they are
 * MISLABELLED, and this rig already treats crediting bits to the wrong
 * combination as the error that matters. */
/* Returns false if the flush did not complete inside ONSET_SETTLE_MS. The
 * caller MUST then produce no measurement.
 *
 * ⚠ A timed-out flush is worse than a late one. camera_ring_flush() drops the
 * ring at the next PAIR BOUNDARY, so if no pair arrives the ring is never
 * dropped at all — the run would then consume exactly the stale bits the flush
 * existed to remove, and nothing downstream could tell. Carrying on silently
 * was the original behaviour and it is the one case where the flush turns from
 * a safeguard into a disguise.
 *
 * VOID is the house rule for this and predates the flush: gcp_zscore_raw()
 * already refuses to return a short run rather than normalise it by a √segments
 * it never reached. A run whose provenance is unknown is treated the same way —
 * archived with k=0, never ranked. It is NOT a camera fault: a pair costs 56 ms
 * idle and 85 ms loaded, so 500 ms is a long stall but well short of
 * CAM_STALL_TIMEOUT_MS, and escalating a transient to a node drop would cost an
 * arm for the rest of the session. */
static bool onset_settle(void)
{
    camera_ring_flush(1);
    int64_t limit = esp_timer_get_time() + ONSET_SETTLE_MS * 1000LL;
    while (!camera_ring_flushed() && esp_timer_get_time() < limit)
        vTaskDelay(1);
    if (camera_ring_flushed()) return true;
    g_status.flush_timeouts++;
    printf("onset flush TIMED OUT after %d ms -- run voided (ring not dropped, "
           "so its bits would be pre-window)\n", ONSET_SETTLE_MS);
    return false;
}

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

    /* ONE run per candidate number (g_status.run_segments), in a fresh
     * random order — every number exactly once, never twice in a row (onset
     * is the payload; no back-to-back reps of the same target).
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
    /* scoring_total is NOT touched here. The caller sets it once, up front, to
     * the whole group's count — 62 for a Eurojackpot loop (50 main + 12 bonus).
     *
     * Two earlier versions of this were both wrong in ways the operator sees.
     * Assigning `n_order` here made the bonus pass reset a full bar to 0/12.
     * Accumulating fixed the reset but left the TOTAL growing mid-phase: the
     * bar read 49/50 and then 54/62, so the percentage jumped backwards. A
     * progress bar whose denominator moves is not a progress bar. */

    for (int idx = 0; idx < n_order; idx++) {
        if (g_status.abort_requested) return;
        pause_gate();
        if (g_status.abort_requested) return;
        int k = order[idx];
        // On screen before the run starts and until it ends — display and
        // bits cover the same interval, or the panel is decoration. ONE window
        // per number (session run_segments): genuine onset, held once.
        focus_show_number(k, euro_pool);
        scores[k] = score_one_run();
        g_status.scoring_done++;
        g_status.elapsed_ms = elapsed_ms_now();
        run_gap_ms(gap_for());
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
    /* Pick by pre-registered score_dir. HIGH = largest z (historical default);
     * LOW = smallest z; ABS = largest |z|. Direction is a session parameter
     * (?score=) so the hypothesis is on the record before the pass. */
    for (int i = n_keep; i < pool_size; i++) {
        int b = 0;
        double bs = 0.0;
        bool first = true;
        for (int j = 1; j <= max_val; j++) {
            if (used[j] || skip[j]) continue;
            double s = scores[j], key;
            switch (g_status.score_dir) {
            case SCORE_DIR_LOW: key = -s;           break;
            case SCORE_DIR_ABS: key = fabs(s);      break;
            default:            key = s;            break;  /* HIGH */
            }
            if (first || key > bs) { b = j; bs = key; first = false; }
        }
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
/* ── The observer gate (PHASE_READY) ───────────────────────────────────
 * Same one-word handshake as the pool prompt: the webserver task sets the flag,
 * elotto_task spins on it. No timeout, deliberately — there is no sensible
 * default action here (the pool prompt has one: take the proposal). A session
 * parked here waits as long as the operator needs, which is only safe because
 * the caller arms it on `focus_mode`: an unattended run has nobody to press
 * Start, so for it the wait would never end. */
static volatile bool s_ready_go = false;

/* Dark time between the operator pressing Start and the first target appearing.
 * The press itself is an act of attention — hand on the mouse, eyes on the
 * button — and the first number's ONSET is what the observer is meant to
 * notice. Without a gap the two overlap, and the first number of the pass is
 * measured while attention is still on the click. One second is the same order
 * as the 350 ms inter-run blank but longer on purpose: this transition also
 * carries the switch from operating the machine to attending to it. */
#define READY_SETTLE_MS  1000

void elotto_ready_go(void) { s_ready_go = true; }

static void ready_wait(void)
{
    s_ready_go     = false;
    g_status.phase = PHASE_READY;
    while (!s_ready_go && !g_status.abort_requested) {
        vTaskDelay(pdMS_TO_TICKS(100));
        g_status.elapsed_ms = elapsed_ms_now();
    }
}

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
        /* Only the numbers actually being re-measured count toward this bar —
         * the retained ones are skipped entirely, so counting them would leave
         * the bar permanently short of its total. */
        g_status.scoring_total = (s_pool_sel_nm < pool_nm ? mx - s_pool_sel_nm : 0)
                               + (euro && s_pool_sel_ne < pool_ne ? 12 - s_pool_sel_ne : 0);
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

/* Gather per-node z for this round and Stouffer-combine. Soft-downweighted
 * nodes stay in `have[]` (pairwise diagnostics still see them) but are
 * skipped in the combine when that still leaves k ≥ 2; otherwise they are
 * used (never force a solo combine that would collapse the scientific floor).
 * Returns k; k == 0 means VOID — caller must not publish as a result.
 * *out_mask receives the bit mask of nodes that actually entered the combine. */
static int gather_and_combine(double z_master, bool master_ok,
                              double znode[MAX_NODES], bool have[MAX_NODES],
                              double *out_z, uint8_t *out_mask)
{
    for (int i = 0; i < MAX_NODES; i++) {
        znode[i] = 0.0;
        have[i]  = false;
    }
    if (master_ok && g_status.nodes[0].ok) {
        znode[0] = z_master;
        have[0]  = true;
    }
    for (int s = 0; s < nodes_slave_count(); s++) {
        double zs = 0.0;
        if (!g_status.nodes[s + 1].ok || !node_take_z(s, &zs)) continue;
        znode[s + 1] = zs;
        have[s + 1]  = true;
    }

    /* Soft-exclude only when at least NODE_SOFT_MIN_COMBINE non-soft nodes
     * answered this run — never collapse the array below three arms. */
    int n_soft = 0;
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (have[i] && !g_status.nodes[i].soft_down) n_soft++;
    }
    bool use_soft = (n_soft >= NODE_SOFT_MIN_COMBINE);

    double sum = 0.0;
    int    k   = 0;
    uint8_t mask = 0;
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (!have[i]) continue;
        if (use_soft && g_status.nodes[i].soft_down) continue;
        sum += znode[i];
        k++;
        mask |= (uint8_t)(1u << i);
    }
    if (out_mask) *out_mask = mask;
    if (out_z) *out_z = (k > 0) ? sum / sqrt((double)k) : 0.0;
    return k;
}

/* One scoring run, node-combined like Phase 2: trigger every node, measure
 * locally in parallel, combine ÷√k. No baseline subtraction — the offset is
 * common to every number and scoring only ranks them. */
static double score_one_run(void)
{
    int nseg = segments_for();               // session window, all phases
    /* Trigger BEFORE settling: every node now flushes its own ring on 'M', so
     * ordering it the other way would leave the master a full settle ahead of
     * the slaves and the "same window" would be offset by ~85 ms. */
    slave_trigger(nseg);
    bool   fresh = onset_settle();
    double zm = 0.0;
    bool   ok = fresh && gcp_zscore_ok(nseg, &zm);
    // Sampling is over the moment the local run returns; the reply wait below
    // is dark time. The caller lit the panel — see focus_publish().
    focus_off();
    if (fresh && !ok) node_camera_failed(0, "stalled mid-run");
    // Timeout from THIS run's length: at the scoring length the old flat 4 s
    // would expire while every slave was still measuring.
    if (nodes_have_slaves()) nodes_collect(LINK_MEAS_MS_FOR(nseg), true);
    double znode[MAX_NODES];
    bool   have[MAX_NODES];
    double z = 0.0;
    uint8_t mask = 0;
    int k = gather_and_combine(zm, ok, znode, have, &z, &mask);
    (void)mask;
    return (k > 0) ? z : 0.0;   /* void scores as 0 for ranking noise only */
}

/* Effective variance of the Stouffer combine under the measured per-node σ
 * and pairwise r: Var(Σz_i/√k) = (Σσ_i² + 2 Σ_{i<j} r_ij σ_i σ_j) / k.
 * Equals 1 when every node is unit-variance and independent. */
static double compute_v_eff(void)
{
    int n = g_status.node_count;
    if (n < 1) return 1.0;
    /* Active (ok and not soft-down) nodes only. */
    int idx[MAX_NODES], k = 0;
    double sig[MAX_NODES];
    for (int i = 0; i < n && i < MAX_NODES; i++) {
        if (!g_status.nodes[i].ok || g_status.nodes[i].soft_down) continue;
        idx[k] = i;
        sig[k] = g_status.nodes[i].sigma > 0.0 ? g_status.nodes[i].sigma : 1.0;
        k++;
    }
    if (k < 1) return 1.0;
    if (k == 1) return sig[0] * sig[0];
    double num = 0.0;
    for (int a = 0; a < k; a++) {
        num += sig[a] * sig[a];
        for (int b = a + 1; b < k; b++) {
            int i = idx[a], j = idx[b];
            double r = (i < j) ? g_status.pair_r[i][j] : g_status.pair_r[j][i];
            num += 2.0 * r * sig[a] * sig[b];
        }
    }
    double v = num / (double)k;
    return (v > 1e-12) ? v : 1.0;
}

/* Refresh null_flags from the current pass health + drift + pair gates. */
static void update_null_flags(void)
{
    uint8_t f = 0;
    int n = g_status.pass_n_valid;
    double sig = g_status.pass_sigma;
    if (n >= PASS_NULL_MIN_N && sig > 0.0) {
        if (sig < PASS_SIGMA_LO || sig > PASS_SIGMA_HI) f |= NULL_FLAG_SIGMA;
        /* χ²/n = (Σz²)/n; under H₀ ≈ 1. Same band as σ when mean≈0; when mean
         * is large, σ can look fine while Σz² is inflated — catch that too. */
        double chi2_over_n = g_status.pass_chi2 / (double)n;
        if (chi2_over_n < PASS_SIGMA_LO * PASS_SIGMA_LO ||
            chi2_over_n > PASS_SIGMA_HI * PASS_SIGMA_HI)
            f |= NULL_FLAG_CHI2;
    }
    if (g_status.loops_done >= 6 && fabs(g_status.drift_t) > DRIFT_FLAG_T)
        f |= NULL_FLAG_DRIFT;
    if (g_status.pair_n > 1) {
        double rz = fabs(g_status.pair_r_max) * sqrt((double)g_status.pair_n);
        if (rz > PAIR_FLAG_T) f |= NULL_FLAG_PAIR;
    }
    g_status.null_flags = f;
}

/* True if this row enters pass mean/σ and Top/Bottom. Void and quarantined
 * trigger-block rows stay in results[] / CSV but not in the ranking. */
/* The value every pass statistic and every ranking runs on: the block-centred
 * combine, not the raw z. One accessor so the choice is made in exactly one
 * place — mixing the two silently would be indistinguishable from a result. */
static double rank_z(const RunResult *r) { return (double)r->z_ctr; }

static bool result_ranked(const RunResult *r)
{
    return r && r->k > 0 && !r->skip_rank;
}

/* Recompute pass mean/σ/χ², studentized Top-N / Bottom-N, and Bonferroni p
 * from the ranked prefix. O(n·TOP_N) after each valid item — exact against
 * the live mean, which is the whole point of Z*. */
static void recompute_pass_ranks(void)
{
    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;

    double sum = 0.0, sumsq = 0.0;
    int    nv  = 0, nvoid = 0, nexcl = 0;
    for (int j = 0; j < ntot; j++) {
        const RunResult *r = &g_status.results[j];
        if (r->k == 0) { nvoid++; continue; }
        if (r->skip_rank) { nexcl++; continue; }
        double z = rank_z(r);
        sum += z;
        sumsq += z * z;
        nv++;
    }
    g_status.pass_n_valid = nv;
    g_status.pass_n_void  = nvoid;
    g_status.pass_n_excl  = nexcl;
    g_status.pass_chi2    = sumsq;
    if (nv <= 0) {
        g_status.pass_mean = g_status.pass_sigma = g_status.pass_stouffer = 0.0;
        g_status.result_count = g_status.low_count = 0;
        g_status.best_z = 0.0;
        g_status.p_corrected = 1.0;
        g_status.comparisons = 0;
        update_null_flags();
        return;
    }
    double mean = sum / (double)nv;
    double ss = sumsq - (double)nv * mean * mean;
    double sigma = (nv > 1 && ss > 0.0) ? sqrt(ss / (double)(nv - 1)) : 0.0;
    g_status.pass_mean     = mean;
    g_status.pass_sigma    = sigma;
    g_status.pass_stouffer = mean * sqrt((double)nv);

    /* Ranking key is the centred z itself — subtracting the pass mean and
     * dividing by σ is monotone, so it reorders nothing; it only sets the scale
     * the UI prints. zmax_abs is kept studentized because the Bonferroni line
     * needs a standard normal deviate.
     *
     * Built into LOCAL lists and published at the end: /status and
     * /results.csv read top[]/low[] from the HTTP task, and zeroing the counts
     * before refilling them let a poll land on an empty or half-built table.
     * The counts still move last, so a racing reader sees either the old list
     * or the new one, never a partial one. */
    RunResult ltop[TOP_N], llow[TOP_N];
    int       ltn = 0, lln = 0;
    double zmax_abs = 0.0;

    for (int j = 0; j < ntot; j++) {
        const RunResult *r = &g_status.results[j];
        if (!result_ranked(r)) continue;
        double z  = rank_z(r);
        double zs = (sigma > 0.0) ? (z - mean) / sigma : (z - mean);
        if (fabs(zs) > zmax_abs) zmax_abs = fabs(zs);

        if (ltn < TOP_N || z > rank_z(&ltop[ltn - 1])) {
            if (ltn < TOP_N) ltn++;
            int p = ltn - 1;
            while (p > 0 && rank_z(&ltop[p - 1]) < z) {
                ltop[p] = ltop[p - 1];
                p--;
            }
            ltop[p] = *r;
        }

        if (lln < TOP_N || z < rank_z(&llow[lln - 1])) {
            if (lln < TOP_N) lln++;
            int p = lln - 1;
            while (p > 0 && rank_z(&llow[p - 1]) > z) {
                llow[p] = llow[p - 1];
                p--;
            }
            llow[p] = *r;
        }
    }

    for (int i = 0; i < ltn; i++) g_status.top[i] = ltop[i];
    for (int i = 0; i < lln; i++) g_status.low[i] = llow[i];
    g_status.result_count = ltn;
    g_status.low_count    = lln;

    /* Bonferroni on |Z*|, comparisons = ranked items only. */
    g_status.comparisons = nv;
    g_status.best_z      = zmax_abs;
    if (nv > 0 && zmax_abs > 0.0) {
        double p1 = erfc(zmax_abs / 1.41421356237);
        double pc = (double)nv * p1;
        if (pc > 1.0) pc = 1.0;
        g_status.p_corrected = pc;
    } else {
        g_status.p_corrected = 1.0;
    }
    update_null_flags();
}

/* Quarantine every measured item in `block_idx` from pass ranking. CSV keeps
 * the rows (skip_rank=1). Called when that block *triggered* a soft-down. */
static void quarantine_block(int block_idx)
{
    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;
    int n = 0;
    for (int j = 0; j < ntot; j++) {
        if ((int)g_status.results[j].block == block_idx &&
            g_status.results[j].k > 0 &&
            !g_status.results[j].skip_rank) {
            g_status.results[j].skip_rank = 1;
            n++;
        }
    }
    if (n > 0)
        printf("pass: quarantine block %d (%d items) — soft-down trigger\n",
               block_idx, n);
}

/* Fold one VALID item into the pass (running sums kept for convenience;
 * ranks always recompute from results[] so mean/σ stay exact). */
static void publish_valid(const RunResult *r)
{
    if (result_ranked(r)) {
        s_pass_sum   += r->z_score;
        s_pass_sumsq += r->z_score * r->z_score;
        s_pass_n++;
    }
    recompute_pass_ranks();
}

/* The items closest to the pass mean — the "nothing happened here" group.
 * Contract and the reason it is recomputed rather than accumulated are in
 * sensor.h; this is the plain two-pass implementation of it. Voids skipped. */
int results_near_mean(RunResult *out, int cap, double *out_mean, double *out_sigma)
{
    if (out_mean)  *out_mean  = 0.0;
    if (out_sigma) *out_sigma = 0.0;
    if (!out || cap <= 0) return 0;

    int n = g_status.runs_completed;
    if (n > NUM_RUNS) n = NUM_RUNS;
    if (n <= 0) return 0;

    double sum = 0.0;
    int    nv  = 0;
    for (int j = 0; j < n; j++) {
        if (!result_ranked(&g_status.results[j])) continue;
        sum += rank_z(&g_status.results[j]);
        nv++;
    }
    if (nv <= 0) return 0;
    double mean = sum / (double)nv;

    // Sample σ (df = n−1), as everywhere else in this file: the mean it is
    // taken about was estimated from the same data.
    double ss = 0.0;
    for (int j = 0; j < n; j++) {
        if (!result_ranked(&g_status.results[j])) continue;
        double d = rank_z(&g_status.results[j]) - mean;
        ss += d * d;
    }
    double sigma = (nv > 1 && ss > 0.0) ? sqrt(ss / (double)(nv - 1)) : 0.0;

    if (out_mean)  *out_mean  = mean;
    if (out_sigma) *out_sigma = sigma;

    // Insertion sort by |z − mean|, keeping the `cap` smallest.
    int got = 0;
    for (int j = 0; j < n; j++) {
        if (!result_ranked(&g_status.results[j])) continue;
        double d = fabs(rank_z(&g_status.results[j]) - mean);
        if (got == cap && d >= fabs(rank_z(&out[got - 1]) - mean)) continue;
        if (got < cap) got++;
        int p = got - 1;
        while (p > 0 && fabs(rank_z(&out[p - 1]) - mean) > d) {
            out[p] = out[p - 1]; p--;
        }
        out[p] = g_status.results[j];
    }
    return got;
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
        /* Live session mean for the node table (Z / p columns). Online update
         * so /status always has a number during a long block, not only after
         * pairs_fold_loop closes it. */
        {
            NodeStatus *N = &g_status.nodes[i];
            N->z_n++;
            N->z_mean += (z[i] - N->z_mean) / (double)N->z_n;
        }
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
    g_status.v_eff      = compute_v_eff();
    update_null_flags();   /* pair flag may have changed */
}

/* ── Cross-block drift instrumentation ─────────────────────────────────
 * v3 publishes RAW z, so an offset drifting across the ~10 h pass lands
 * directly in the numbers — random measurement order stops it attaching to
 * particular combinations, but it still widens the extremes. This regression
 * is therefore the first diagnostic to read on a v3 session, not a footnote:
 * the master's raw per-run offset per BLOCK against the block index, slope +
 * t published. Running sums, so it stays exact past LOOP_HIST stored blocks. */
static struct { double n, sx, sxx, sy, sxy, syy; } s_drift;

/* Blocks needed before the slope is published at all.
 *
 * SIX, not three. The regression has n−2 degrees of freedom, so at three
 * points t has ONE — where the 5 % critical value is 12.7, not 2. The 10-loop
 * session of 2026-07-26 duly reported drift_t = +10.30 after loop 3 (p ≈ 0.06,
 * i.e. nothing), then settled to −0.20 by loop 10 as the df arrived. A flag
 * that fires on noise is worse than no flag. At the 15-min default a full
 * Eurojackpot pass closes ~38 blocks, so this floor clears in ~1.5 h. */
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
    update_null_flags();   /* drift gate may have flipped */
}

/* Append one closed block to the health table. Must run BEFORE
 * pairs_fold_loop(), which clears the per-block sums this reads. */
static void record_loop(double loop_mean, int loop_idx)
{
    // Per-node mean and σ for this block, straight from the un-folded sums.
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
        // 0 when this loop skipped the sweep — g_status.cal_ms still holds the
        // last sweep's duration (the UI estimates its progress bar from it), so
        // copying it unconditionally would log a sweep that never ran here.
        L->cal_ms = g_status.cal_did_sweep
                  ? (uint16_t)(g_status.cal_ms > 65535 ? 65535 : g_status.cal_ms)
                  : 0;
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
    // The master's RAW per-run offset this block. v3 subtracts nothing from
    // the master's z (znode[0] IS zraw), so mean_n[0] already carries the
    // whole offset — adding baseline_mean on top would double-count it. The
    // baseline survives separately in LoopStat.base as the cross-check: two
    // instruments estimating the same offset should agree within their SEs.
    double raw_off = mean_n[0];
    if (g_status.loops_done == 0) g_status.off_first = raw_off;
    g_status.off_last = raw_off;
    g_status.loops_done++;

    drift_add((double)loop_idx, raw_off);

    /* Soft downweight — sticky, min-k protected, trigger-block quarantine.
     *
     * Trip (either wire): σ > NODE_SIGMA_SOFT  OR  |mean| > NODE_MEAN_SOFT.
     * Clear: NODE_SOFT_CLEAR_BLOCKS consecutive clean blocks (not one lucky
     * block — the 2026-08-11 full pass re-admitted the master after quiet
     * blocks and then block 9 poisoned the ranking), where "clean" is measured
     * against the PEERS in that same block and no longer against fixed
     * constants — see NODE_SOFT_CLEAR_* in sensor.h for why the constants were
     * unreachable in practice.
     * Floor: never soft-exclude so many that fewer than NODE_SOFT_MIN_COMBINE
     * ok nodes remain eligible; keep the least-bad among candidates.
     * Quarantine: the block that *triggered* a new soft-down is excluded from
     * pass mean/σ/Top-Bottom (CSV still holds every row). Never reboots. */
    bool   want[MAX_NODES] = {false};
    double score[MAX_NODES] = {0};   /* higher = worse; for triage when floor binds */
    bool   have_stats[MAX_NODES] = {false};
    double mean_i[MAX_NODES] = {0}, sig_i[MAX_NODES] = {0};
    bool   any_trip = false;

    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        const NodeAcc *a = &s_nacc[i];
        if (a->n < NODE_SOFT_MIN_N) continue;
        double n = (double)a->n, m_i = a->s / n;
        double v = (a->ss - n * m_i * m_i) / (n - 1.0);
        double sig = v > 0.0 ? sqrt(v) : 0.0;
        mean_i[i] = m_i;
        sig_i[i]  = sig;
        have_stats[i] = true;
        bool bad_sig  = (sig > NODE_SIGMA_SOFT);
        bool bad_mean = (fabs(m_i) > NODE_MEAN_SOFT);
        if (bad_sig || bad_mean) {
            want[i] = true;
            double sm = bad_mean ? fabs(m_i) / NODE_MEAN_SOFT : 0.0;
            double ss = bad_sig  ? sig / NODE_SIGMA_SOFT : 0.0;
            score[i] = sm > ss ? sm : ss;
        }
    }

    /* How many ok nodes would remain if every want[] were soft-downed? */
    int n_ok = 0;
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++)
        if (g_status.nodes[i].ok) n_ok++;
    int n_want = 0;
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++)
        if (want[i] && g_status.nodes[i].ok) n_want++;
    int remain = n_ok - n_want;
    /* Already-soft nodes that are not in want stay soft; count them out. */
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (g_status.nodes[i].ok && g_status.nodes[i].soft_down && !want[i])
            remain--;
    }
    if (remain < NODE_SOFT_MIN_COMBINE && n_want > 0) {
        /* Keep the least-bad among want[] until remain >= floor. */
        int need_keep = NODE_SOFT_MIN_COMBINE - remain;
        while (need_keep > 0) {
            int best = -1;
            double best_sc = 1e300;
            for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
                if (!want[i]) continue;
                if (score[i] < best_sc) { best_sc = score[i]; best = i; }
            }
            if (best < 0) break;
            want[best] = false;
            need_keep--;
        }
    }

    /* The clear bars for THIS block, taken from the peers measured in it. A node
     * that is down is judged against the arms that are up, in the same window,
     * instead of against a constant that turned out to be the array's own
     * median. See NODE_SOFT_CLEAR_* in sensor.h for what that cost. */
    double peer_sig[MAX_NODES], peer_mean[MAX_NODES];
    int    npeer = 0;
    for (int j = 0; j < g_status.node_count && j < MAX_NODES; j++) {
        if (!g_status.nodes[j].ok || !have_stats[j]) continue;
        if (want[j] || g_status.nodes[j].soft_down) continue;   /* suspect: no reference */
        peer_sig[npeer]  = sig_i[j];
        peer_mean[npeer] = fabs(mean_i[j]);
        npeer++;
    }
    double clear_sig = NODE_SOFT_CLEAR_SIG, clear_mean = NODE_SOFT_CLEAR_MEAN;
    if (npeer > 0) {
        for (int a = 1; a < npeer; a++) {          /* insertion sort, n <= 4 */
            double vs = peer_sig[a], vm = peer_mean[a];
            int b = a - 1;
            while (b >= 0 && peer_sig[b] > vs)  { peer_sig[b + 1]  = peer_sig[b];  b--; }
            peer_sig[b + 1] = vs;
            b = a - 1;
            while (b >= 0 && peer_mean[b] > vm) { peer_mean[b + 1] = peer_mean[b]; b--; }
            peer_mean[b + 1] = vm;
        }
        double med_sig  = (npeer & 1) ? peer_sig[npeer / 2]
                                      : 0.5 * (peer_sig[npeer / 2 - 1] + peer_sig[npeer / 2]);
        double med_mean = (npeer & 1) ? peer_mean[npeer / 2]
                                      : 0.5 * (peer_mean[npeer / 2 - 1] + peer_mean[npeer / 2]);
        double cs = med_sig  * NODE_SOFT_CLEAR_SIG_K;
        double cm = med_mean * NODE_SOFT_CLEAR_MEAN_K;
        if (cs > clear_sig)  clear_sig  = cs;      /* floors: never tighter than before */
        if (cm > clear_mean) clear_mean = cm;
        /* ...and never as loose as the TRIP bar, or a block that trips the node
         * could also be counted as a clean one. */
        if (clear_sig  > NODE_SIGMA_SOFT * NODE_SOFT_CLEAR_MARGIN)
            clear_sig  = NODE_SIGMA_SOFT * NODE_SOFT_CLEAR_MARGIN;
        if (clear_mean > NODE_MEAN_SOFT * NODE_SOFT_CLEAR_MARGIN)
            clear_mean = NODE_MEAN_SOFT * NODE_SOFT_CLEAR_MARGIN;
    }
    /* Printed every block because the bars MOVE now: without them in the log a
     * clean / not-clean call cannot be checked after the fact. */
    printf("soft-down clear bars this block: |mean|<=%.3f sigma<=%.3f (from %d peer(s))\n",
           clear_mean, clear_sig, npeer);

    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (!have_stats[i] && !g_status.nodes[i].soft_down) continue;

        if (want[i]) {
            /* Quarantine on every trip that could actually have contaminated
             * this block — i.e. the node was IN the combine while it misbehaved
             * (2026-08-13, corrected same day). Two failure modes, both real:
             *
             *  - Only-the-first-trip (the original rule) waved through every
             *    later bad block, because the node was already soft_down and
             *    nothing re-fired.
             *  - Every-trip-unconditionally quarantines every block for as long
             *    as a sticky node keeps failing its gate — measured live: 33 of
             *    33 items excluded per block, three blocks running, pass σ 0.000,
             *    nothing left to rank at all.
             *
             * A soft-excluded node's excursion never entered the numbers, so
             * there is nothing to quarantine; one that was still combining did
             * contaminate them. s_blk_contrib is the mask of nodes that really
             * entered this block's combines. */
            bool contaminated = (s_blk_contrib & (1u << i)) != 0;
            printf("node %d: soft-down %s (block mean=%.3f σ=%.3f)%s\n", i,
                   g_status.nodes[i].soft_down ? "still tripped" : "tripped (sticky)",
                   mean_i[i], sig_i[i],
                   contaminated ? " — block quarantined" : " — was already out, block kept");
            if (contaminated) any_trip = true;
            g_status.nodes[i].soft_down = 1;
            s_soft_clean[i] = 0;
        } else if (g_status.nodes[i].soft_down && have_stats[i]) {
            bool clean = (sig_i[i] > 0.0 && sig_i[i] <= clear_sig &&
                          fabs(mean_i[i]) <= clear_mean);
            if (clean) {
                if (s_soft_clean[i] < 255) s_soft_clean[i]++;
                if (s_soft_clean[i] >= NODE_SOFT_CLEAR_BLOCKS) {
                    printf("node %d: soft-down cleared after %d clean blocks "
                           "(mean=%.3f σ=%.3f)\n",
                           i, (int)s_soft_clean[i], mean_i[i], sig_i[i]);
                    g_status.nodes[i].soft_down = 0;
                    s_soft_clean[i] = 0;
                } else {
                    printf("node %d: soft-down clean %d/%d (mean=%.3f σ=%.3f)\n",
                           i, (int)s_soft_clean[i], NODE_SOFT_CLEAR_BLOCKS,
                           mean_i[i], sig_i[i]);
                }
            } else {
                printf("node %d: soft-down streak broken at %d/%d "
                       "(mean=%.3f sigma=%.3f)\n",
                       i, (int)s_soft_clean[i], NODE_SOFT_CLEAR_BLOCKS,
                       mean_i[i], sig_i[i]);
                s_soft_clean[i] = 0;   /* streak broken */
            }
        }
    }

    if (any_trip) {
        quarantine_block(loop_idx);
        recompute_pass_ranks();
    }
}


/* Re-derive z_ctr for every item of a just-finished block by subtracting each
 * node's own mean over that block, then recombining over the SAME nodes that
 * produced z_score (have_mask, so k is unchanged and the √k scaling still
 * holds). Runs once per block close, while s_nacc still holds this block's
 * per-node sums and before pairs_fold_loop() clears them.
 *
 * Centring is what the 08-13 pass argued for: inside a block every node sat at
 * σ ≈ 1.0, but the block offsets moved by several z. Subtracting a mean
 * estimated from ~103 items costs one degree of freedom per node per block —
 * negligible — and removes the offset exactly.
 *
 * A node with fewer than 2 runs in the block gets no correction (its mean would
 * be the single value itself, which would zero that node's contribution). If
 * the PSRAM archive is missing there is nothing to recompute from, and z_ctr
 * simply stays at the provisional raw value. */
static void center_block(int block_idx)
{
    /* Contribution mask first: it comes from have_mask in results[], not from
     * the archive, so it is still correct when PSRAM was unavailable and no
     * centring can happen. */
    int nt = g_status.runs_completed;
    if (nt > NUM_RUNS) nt = NUM_RUNS;
    s_blk_contrib = 0;
    for (int j = 0; j < nt; j++) {
        const RunResult *r = &g_status.results[j];
        if ((int)r->block == block_idx && r->k > 0) s_blk_contrib |= r->have_mask;
    }

    if (!s_node_z) return;

    double m[MAX_NODES] = {0};
    bool   ok[MAX_NODES] = {false};
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (s_nacc[i].n >= 2) {
            m[i]  = s_nacc[i].s / (double)s_nacc[i].n;
            ok[i] = true;
        }
    }

    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;
    int n = 0;
    for (int j = 0; j < ntot; j++) {
        RunResult *r = &g_status.results[j];
        if ((int)r->block != block_idx || r->k == 0) continue;
        const float *row = s_node_z + (size_t)j * MAX_NODES;
        double sum = 0.0;
        int    kk  = 0;
        for (int i = 0; i < MAX_NODES; i++) {
            if (!(r->have_mask & (1u << i))) continue;
            if (isnan(row[i])) continue;
            sum += (double)row[i] - (ok[i] ? m[i] : 0.0);
            kk++;
        }
        if (kk > 0) {
            r->z_ctr = (float)(sum / sqrt((double)kk));
            n++;
        }
    }
    if (n > 0)
        printf("block %d: centred %d items (node means %+.3f %+.3f %+.3f %+.3f)\n",
               block_idx, n, m[0], m[1], m[2], m[3]);
}

/* ── Block close (v3) ──────────────────────────────────────────────────
 * A block is the span between two sweep+baseline insertions. Closing one
 * turns its running sums into the per-block σ, appends the /loops row, feeds
 * the drift regression and folds the pairwise moments — everything that used
 * to happen at a loop boundary, now on a wall-clock cadence. Nothing here
 * touches results[]: the measured z's are final the moment they are stored. */
static void close_block(int block_idx)
{
    double m = 0.0, s = 0.0;
    if (s_blk_n >= 4) {
        m = s_blk_sum / s_blk_n;
        double v = (s_blk_sumsq - s_blk_n * m * m) / (s_blk_n - 1);
        s = v > 0.0 ? sqrt(v) : 0.0;
    }
    g_status.loop_sigma = s;             // "last closed block" in /status
    /* Centre first: record_loop may quarantine this block and re-rank, and the
     * ranking must already see the centred values. Both read s_nacc, which
     * pairs_fold_loop() clears, so both must run before it. */
    center_block(block_idx);
    record_loop(m, block_idx);           // before the fold clears the sums
    recompute_pass_ranks();              // centring moved every item in the block
    pairs_fold_loop();
    publish_pair_stats();
    s_blk_sum = s_blk_sumsq = 0.0;
    s_blk_n   = 0;
}

/* One baseline pass at measurement length — the drift reference, nothing
 * else: v3 subtracts baseline_mean from NOTHING (with raw z published, a
 * master-only subtraction would be a live asymmetric correction, the trap
 * RANK_EXTREME_RAW documented). It exists to give LoopStat.base its number
 * and the operator an independent estimate of the master's offset.
 * Returns false when the session must stop. */
static bool baseline_run(void)
{
    g_status.phase         = PHASE_BASELINE;
    g_status.baseline_done = 0;
    slave_baseline_start(g_status.baseline_total, segments_for());
    double bsum = 0.0;
    int    bn   = 0;
    for (int i = 0; i < g_status.baseline_total; i++) {
        if (g_status.abort_requested) { slave_abort(); return false; }
        /* The baseline flushes too (2026-08-19). It is not cosmetic consistency:
         * LoopStat.base is the INDEPENDENT cross-check against the block's own
         * master mean (raw_m), and two instruments only estimate the same offset
         * if their bits have the same provenance. That cross-check is what said
         * the 08-19 block-3 excursion was present before the block began
         * (base −1,802 against a block mean of −1,712); a baseline drawing on
         * pre-window ring bits while the runs do not would blunt it. */
        if (!onset_settle()) { run_gap_ms(gap_for()); continue; }
        double bz = 0.0;
        if (gcp_zscore_ok(segments_for(), &bz)) { bsum += bz; bn++; }
        else {
            // A void run must not be averaged in as a zero — that would pull
            // the reference toward 0 and misstate the offset it records.
            node_camera_failed(0, "stalled during baseline");
            break;
        }
        run_gap_ms(gap_for());      // same duty cycle as the runs around it
        g_status.baseline_done = i + 1;
        g_status.elapsed_ms    = elapsed_ms_now();
    }
    g_status.baseline_mean = bn ? bsum / bn : 0.0;
    if (nodes_have_slaves() && !g_status.abort_requested)
        slave_baseline_wait();
    return !g_status.abort_requested;
}

/* ── The session (v3.0, PLAN.md §2): ONE pass, no loops ────────────────
 *
 * calibrate + baseline → observer gate → Phase 0 scoring → pool confirm →
 * every combination in the confirmed pool measured EXACTLY ONCE, ~5 s per
 * item, in one Fisher–Yates random order. Every cal_interval_ms (default
 * 15 min) the pass parks for a sweep + baseline insertion; that boundary
 * closes a block, the unit the drift/pairwise diagnostics run on.
 *
 * results[] fills in MEASUREMENT order (results[j] = j-th item measured, its
 * combination id in .index), so the prefix [0..runs_completed) is always the
 * complete record: publishing, /results.csv and an abort all read it
 * directly, and no compaction step exists any more. */
void elotto_task(void *pvParam)
{
    /* BEFORE the state goes RUNNING, which is the moment /status starts serving
     * the pool: the previous SESSION's numbers must not survive into this one.
     * Everything up to the first scoring pass — discovery, the opening sweep,
     * the baseline, the observer gate — would otherwise show them, and there is
     * nothing on screen to say they are stale. */
    g_status.pool_n_main     = 0;
    g_status.pool_n_euro     = 0;

    g_status.state           = ELOTTO_RUNNING;
    g_status.runs_completed  = 0;
    // unlimited / runs_cap are session parameters from /start and are NOT reset
    // here — they tag the session, exactly like focus_mode.
    g_status.round           = 0;
    g_status.round_base      = 0;
    g_status.round_total     = 0;
    g_status.baseline_done   = 0;
    g_status.scoring_done    = 0;
    g_status.abort_requested = false;
    g_status.elapsed_ms      = 0;
    g_status.baseline_mean   = 0.0;
    g_status.slave_connected = false;
    g_status.result_count    = 0;
    g_status.low_count       = 0;
    g_status.best_z          = 0.0;
    g_status.p_corrected     = 1.0;
    g_status.comparisons     = 0;
    g_status.pass_mean       = 0.0;
    g_status.pass_sigma      = 0.0;
    g_status.pass_chi2       = 0.0;
    g_status.pass_stouffer   = 0.0;
    g_status.pass_n_valid    = 0;
    g_status.pass_n_void     = 0;
    g_status.pass_n_excl     = 0;
    g_status.v_eff           = 1.0;
    g_status.null_flags      = 0;
    memset(s_soft_clean, 0, sizeof(s_soft_clean));
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
    g_status.cal_did_sweep   = false;
    // A new session must not inherit the previous one's calibration age: the
    // rig may have been idle for hours, and the opening insertion is where the
    // operating point has to be established rather than assumed.
    calibrate_forget();
    memset(&s_drift, 0, sizeof(s_drift));
    s_blk_sum = s_blk_sumsq = 0.0;  s_blk_n = 0;
    s_pass_sum = s_pass_sumsq = 0.0; s_pass_n = 0;
    /* Per-node z archive in PSRAM (results[] is full of internal RAM). */
    if (!s_node_z)
        s_node_z = heap_caps_malloc((size_t)NUM_RUNS * MAX_NODES * sizeof(float),
                                    MALLOC_CAP_SPIRAM);
    if (s_node_z) {
        for (size_t i = 0; i < (size_t)NUM_RUNS * MAX_NODES; i++)
            s_node_z[i] = NAN;
    }
    // focus_mode is set by /start and NOT reset here — it is the session's tag.
    focus_reset();
    // Block history lives in PSRAM: results[] already fills internal RAM. Kept
    // for the lifetime of the app (allocated once, never freed) so the table
    // of a finished session survives for inspection.
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
    if (g_status.baseline_total <= 0 || g_status.baseline_total > BASELINE_MAX)
        g_status.baseline_total = BASELINE_DEFAULT;

    // Re-discover every session: a node that was powered off last time joins
    // this one, and one that vanished does not sit in the table as a phantom.
    nodes_discover();

    // After abort_requested is cleared, so a camera that is not ready can abort
    // the session immediately instead of having the flag wiped by the reset above.
    camera_source_begin();

    bool euro    = (g_status.mode == MODE_EUROJACKPOT);
    int  nm      = euro ? 5 : 6;
    int  mx      = euro ? 50 : 49;

    g_status.scoring_total = mx + (euro ? 12 : 0);   // one run per number

    uint8_t pool_main[POOL_MAIN_49] = {0};   // 15 slots, enough for both modes
    uint8_t pool_euro[POOL_EURO_12] = {0};
    // Pool sizes are variables, not constants: attended confirmation can shrink
    // either, unlimited mode derives both from the per-round run cap, and every
    // count downstream is derived from them.
    int     pool_nm = euro ? POOL_MAIN_50 : POOL_MAIN_49;
    int     pool_ne = euro ? POOL_EURO_12 : 0;
    if (g_status.unlimited) {
        if (g_status.runs_cap < 1 || g_status.runs_cap > UNLIM_RUNS_MAX)
            g_status.runs_cap = UNLIM_RUNS_DEFAULT;
        unlimited_pool_sizes(euro, nm, g_status.runs_cap, &pool_nm, &pool_ne);
    }
    g_status.runs_total = comb(pool_nm, nm) * (euro ? comb(pool_ne, 2) : 1);

    // Pairwise independence check across all nodes (per-block centered)
    pairs_reset();
    session_clock_start();

    /* ── Opening insertion: camera sweep + baseline ────────────────────
     * Before anything is displayed. The sweep establishes each node's
     * operating point; the baseline records the offset reference at that
     * same operating point. */
    g_status.cal_did_sweep = calibrate_all();
    if (g_status.abort_requested) { slave_abort(); goto done; }
    if (!baseline_run()) goto done;

    /* ── The observer gate ─────────────────────────────────────────────
     * Everything up to here is the instrument preparing itself, with nothing
     * displayed and nothing to attend to. Scoring is the first phase whose
     * bits are collected while a target is on screen, so it is where the
     * protocol actually begins. Only when a human asked for it.
     *
     * ⚠ Requires focus_mode, not just pool_confirm. This gate has NO timeout
     * by design — there is no sensible default for "did the observer start
     * attending?" — so arming it without an observer parks the pass forever.
     * The web UI sends confirm=1 unconditionally, which is what put a 5005-item
     * unattended run behind a Start button nobody was there to press. No
     * observer, no observer gate. */
    if (g_status.pool_confirm && g_status.focus_mode && !g_status.abort_requested) {
        ready_wait();
        if (g_status.abort_requested) goto done;
        /* Settle. The phase moves off PHASE_READY *first*: the UI re-raises
         * the Start overlay on any /status poll that still sees "ready". */
        g_status.phase = PHASE_SCORING;
        vTaskDelay(pdMS_TO_TICKS(READY_SETTLE_MS));
        g_status.elapsed_ms = elapsed_ms_now();
    }

    /* ── Rounds ────────────────────────────────────────────────────────
     * An ordinary v3 session is ONE round: score, confirm the pool, measure
     * every combination of it exactly once, done.
     *
     * Unlimited mode repeats that indefinitely (user, 2026-08-18). Each round
     * re-scores every number from scratch and keeps only as many of the best as
     * fit `runs_cap` measurement runs, so the pool is small and the whole space
     * of THAT pool is measured before the next round re-picks it. Rounds stop
     * on Abort or when results[] is full.
     *
     * results[] keeps filling across rounds — nothing is cleared between them —
     * so every ranking, the pass mean/σ and the Bonferroni line run on the
     * union of all rounds measured so far, which is what "new results are sorted
     * in" means. Each round is closed as its own block (or blocks), so block
     * centring never mixes items from either side of a re-scoring. */
    int     block          = 0;
    int     round          = 0;
    bool    space_full     = false;
    int64_t last_insert_us = esp_timer_get_time();

    for (;;) {
        round++;
        g_status.round = round;
        /* Withdraw the previous round's pool HERE, at the boundary — not further
         * down where the scoring pass starts. The sweep + baseline insertion
         * sits between the two and takes a minute or more, and `round` has
         * already advanced: the info line would spend that whole time showing
         * the PREVIOUS round's numbers under the NEW round number, which is
         * exactly the reading error withholding it is meant to prevent. */
        g_status.pool_n_main = g_status.pool_n_euro = 0;

        /* Rounds after the first re-establish the operating point before scoring,
         * exactly as the session start does — the scoring runs choose the pool, so
         * they should not be the ones measured on a stale sweep. Skipped when the
         * operator turned mid-pass insertions off (?calint=0), which is that
         * control's whole point. */
        if (round > 1 && g_status.cal_interval_ms > 0) {
            g_status.cal_did_sweep = calibrate_all();
            if (g_status.abort_requested) { slave_abort(); goto done; }
            if (!baseline_run()) goto done;
        }

        /* ── Phase 0: individual number scoring, once per round ──────────── */
        g_status.phase = PHASE_SCORING;
        // The WHOLE group's count, up front: 50 main + 12 bonus = 62 for
        // Eurojackpot. Both passes fill one bar that counts to 62. Every number is
        // scored regardless of how many the pool will keep, so this is the same
        // work in unlimited mode as in a full pass.
        g_status.scoring_total = mx + (euro ? 12 : 0);
        g_status.scoring_done  = 0;
        /* Unlimited: the pool sizes come from the run cap, and are re-derived every
         * round because the cap is fixed while nothing else here is. */
        if (g_status.unlimited)
            unlimited_pool_sizes(euro, nm, g_status.runs_cap, &pool_nm, &pool_ne);
        /* The pool is already withdrawn — at the top of the round, and at
         * session start before the state went RUNNING. Nothing to clear here. */
        memset(pool_main, 0, sizeof(pool_main));
        memset(pool_euro, 0, sizeof(pool_euro));
        score_and_build_pool(mx, pool_nm, pool_main, false,
                             NULL, NULL, 0, g_status.pool_main_z);
        if (g_status.abort_requested) goto done;
        if (euro) score_and_build_pool(12, pool_ne, pool_euro, true,
                                       NULL, NULL, 0, g_status.pool_euro_z);
        if (g_status.abort_requested) goto done;
        focus_off();

        if (g_status.unlimited) {
            /* No confirmation gate: choosing the pool by score is what the mode
             * IS, and a round boundary arrives every few minutes. Recorded as
             * pool_auto=1, like every other selection no human approved. */
            g_status.pool_auto = 1;
        } else if (g_status.pool_confirm) {
            /* ── Attended pool confirmation ────────────────────────────────
             * Stop and show the operator what scoring chose, before ~10 h are
             * spent measuring it. Only when the session asked for it.
             *
             * Unattended: take the proposal at once instead of burning the
             * 15-minute timeout waiting for an operator who is not there.
             * Recorded as pool_auto=1, exactly as the timeout path records it —
             * the CSV must not be able to claim a human approved this pool. */
            if (!g_status.focus_mode) {
                g_status.pool_auto = 1;
                printf("pool: unattended session — proposal taken unchanged\n");
            } else {
                pool_confirm_wait(euro, mx, &pool_nm, &pool_ne,
                                  pool_main, pool_euro, nm);
            }
            if (g_status.abort_requested) goto done;
        }

        /* Publish the pool that is actually about to be measured — EVERY path,
         * not just the confirmation gate. It used to be written only while the
         * operator was being asked about it, so a session started without
         * ?confirm= measured a pool /status never named. The UI shows it under
         * the scoring bar for the whole run, and in unlimited mode it is the one
         * thing that changes from round to round. */
        g_status.pool_need_main = (uint8_t)nm;
        g_status.pool_need_euro = euro ? 2 : 0;
        for (int i = 0; i < pool_nm; i++) g_status.pool_main[i] = pool_main[i];
        for (int i = 0; i < pool_ne; i++) g_status.pool_euro[i] = pool_euro[i];
        g_status.pool_n_main    = (uint8_t)pool_nm;
        g_status.pool_n_euro    = (uint8_t)pool_ne;

        /* Everything derived from the pool sizes — the operator may have removed
         * numbers, and in unlimited mode the sizes are the cap's answer. At the
         * minimum (pool == draw size) this is exactly ONE combination. */
        int main_combos = comb(pool_nm, nm);
        int euro_combos = euro ? comb(pool_ne, 2) : 1;
        int full_combos = main_combos * euro_combos;
        /* s_perm is NUM_RUNS wide and the shuffle below now indexes the WHOLE
         * space, so the space must fit it. Euro 12+5 = 7920 and 6-of-49 pool 15
         * = 5005 are both under NUM_RUNS 8000; this is the guard, not a case
         * that is reachable today. */
        if (full_combos > NUM_RUNS) full_combos = NUM_RUNS;

        /* Where this round lands in results[], and how much room is left. The
         * buffer is the hard stop for unlimited mode: a truncated round is still
         * a valid measured prefix, but it is the last one. */
        g_status.round_base = g_status.runs_completed;
        int room = NUM_RUNS - g_status.round_base;
        int round_total = full_combos;
        if (round_total > room) { round_total = room; space_full = true; }
        if (round_total <= 0) { space_full = true; break; }
        g_status.round_total = round_total;
        g_status.runs_total  = round_total;

        /* ── The pass: every combination of THIS round's pool once, in random
         * order ────────────────────────────────────────────────────────────
         * Fisher–Yates over the round's space. With a fixed order, slow drift
         * over the pass would map onto the enumeration order and read as
         * structure in the numbers; randomized, it spreads evenly.
         * fast_rng() deliberately, not the camera: measurement order is
         * administrative randomness and must not spend camera entropy. */
        /* Draw a uniformly random SUBSET of the space, not its first entries.
         *
         * This used to fill s_perm with 0..round_total-1 and shuffle those among
         * themselves, which randomises the ORDER but not the SELECTION: when a
         * round is truncated, the ids measured were exactly the lexicographically
         * first ones -- in 6-of-49 the combinations built from the pool's lowest
         * numbers, and in Eurojackpot, where mi = i % main_combos and
         * ei = i / main_combos, a truncation below main_combos pinned ei to the
         * FIRST euro pair for the whole round.
         *
         * Only the LAST round of an unlimited session truncates, i.e. only where
         * results[] fills at NUM_RUNS -- the one path CLAUDE.md still listed as
         * never exercised, which is why this survived.
         *
         * Forward partial Fisher-Yates: after k steps s_perm[0..k-1] holds a
         * uniformly random k-subset in random order, and at k == full_combos it
         * degenerates to an ordinary full shuffle, so the untruncated round (every
         * round but the last) is unchanged in distribution. */
        for (int i = 0; i < full_combos; i++) s_perm[i] = (uint16_t)i;
        for (int i = 0; i < round_total; i++) {
            int j = i + (int)(fast_rng() % (uint32_t)(full_combos - i));
            uint16_t t = s_perm[i]; s_perm[i] = s_perm[j]; s_perm[j] = t;
        }

        last_insert_us = esp_timer_get_time();
        g_status.phase = PHASE_MEASURING;
        for (int j = 0; j < round_total; j++) {
            if (g_status.abort_requested) { slave_abort(); goto done; }
            // Between runs, never inside one: the run just finished is kept and
            // j does not advance, so nothing is lost or duplicated.
            pause_gate();
            if (g_status.abort_requested) { slave_abort(); goto done; }

            /* ── Block boundary: sweep + baseline every cal_interval_ms ────
             * Wall-clock, like the sweep trigger always was (§1.18) — 0 means
             * no mid-pass insertions at all. Closing the block BEFORE the
             * insertion keeps the /loops row describing items measured at ONE
             * operating point; the insertion then opens the next block. */
            if (j > 0 && g_status.cal_interval_ms > 0 &&
                (esp_timer_get_time() - last_insert_us) / 1000
                    >= (int64_t)g_status.cal_interval_ms) {
                close_block(block);
                block++;
                g_status.cal_did_sweep = calibrate_all();
                if (g_status.abort_requested) { slave_abort(); goto done; }
                if (!baseline_run()) goto done;
                last_insert_us = esp_timer_get_time();
                g_status.phase = PHASE_MEASURING;
            }

            /* Uncapped and unrepeated within the round, so slot i IS this
             * round's combination i. */
            int i  = s_perm[j];
            int mi = i % main_combos;
            int ei = euro ? (i / main_combos) : 0;
            int slot = g_status.round_base + j;       // measurement order, session-wide
            RunResult *r = &g_status.results[slot];
            nth_combination(pool_main, pool_nm, nm, mi, r->nums);
            if (euro)
                nth_combination(pool_euro, pool_ne, 2, ei, r->euro);
            else
                r->euro[0] = r->euro[1] = 0;

            // The draw goes on screen BEFORE the trigger, so the observer is
            // already attending when the first bit is sampled.
            focus_publish(FOCUS_DRAW, r->nums, nm, r->euro, euro ? 2 : 0);

            // One broadcast starts every node, then measure locally — all of
            // them integrate the same window, which is the premise the sqrt(n)
            // combine rests on. EVERY session flushes the ring first, attended
            // or not, so the bits credited to this item were captured during it.
            // Trigger first: the slaves flush on 'M', so settling before the
            // broadcast would put the master a full settle ahead of them.
            int nseg = segments_for();
            slave_trigger(nseg);
            bool   fresh = onset_settle();
            double zraw = 0.0;
            /* No flush, no measurement: the local z is withheld rather than
             * computed from bits of unknown provenance. The slaves were already
             * triggered and answer normally; each one withholds its own z the
             * same way if ITS flush timed out, so k simply drops. */
            bool   zok  = fresh && gcp_zscore_ok(nseg, &zraw);
            focus_off();          // sampling done; the reply wait is dark time
            if (fresh && !zok) node_camera_failed(0, "stalled mid-run");
            if (nodes_have_slaves()) nodes_collect(LINK_MEAS_MS_FOR(nseg), true);

            double znode[MAX_NODES];
            bool   have[MAX_NODES];
            double z = 0.0;
            uint8_t mask = 0;
            int k = gather_and_combine(zraw, zok, znode, have, &z, &mask);

            /* Archive every node that produced a z (including soft-down ones),
             * so post-hoc recombine is possible. */
            node_z_store(slot, znode, have);
            pairs_add_run(znode, have);

            r->index     = i + 1;
            r->block     = (uint16_t)block;
            r->round     = (uint16_t)round;
            r->k         = (uint8_t)k;
            r->have_mask = mask;
            r->skip_rank = 0;   /* quarantine only at block close on soft-down trip */
            if (k > 0) {
                r->z_score = z;
                /* Provisional: the block's node means are not known until it
                 * closes, so the live ranking uses the uncentred value and
                 * center_block() replaces it a few minutes later. */
                r->z_ctr   = (float)z;
                s_blk_sum += z;  s_blk_sumsq += z * z;  s_blk_n++;
            } else {
                /* VOID: incomplete combine. Archived with k=0, never enters
                 * ranking, Bonferroni, pass mean/σ, or block σ. */
                r->z_score = 0.0;
                r->z_ctr   = 0.0f;
            }

            g_status.runs_completed = slot + 1;  // AFTER the row is complete: readers
                                                 // (/status, /results.csv) trust the prefix
            if (k > 0) publish_valid(r);      // studentized top/low + pass health
            g_status.elapsed_ms     = elapsed_ms_now();
            run_gap_ms(gap_for());
        }
        /* The round's final block. Always closed here, even with ?calint=0, so a
         * round is never centred together with the one after it — the pool changed
         * in between, and scoring sat between the two. */
        close_block(block);
        block++;

        if (!g_status.unlimited) break;
        if (g_status.abort_requested) break;
        if (space_full || g_status.runs_completed >= NUM_RUNS) {
            snprintf(g_status.fault, sizeof(g_status.fault),
                     "unlimited: results buffer full (%d items) after round %d "
                     "— session ended, pull /results.csv?all=1",
                     g_status.runs_completed, round);
            printf("%s\n", g_status.fault);
            break;
        }
    }
    goto finalize;

done:
    /* Aborted. results[0..runs_completed) is already compact and already
     * published item by item — nothing to recompute except folding the
     * partial block's pairwise moments so the matrix stays complete. The
     * partial block is deliberately NOT given a /loops row: every stored row
     * describes a full between-insertions span. */
    pairs_fold_loop();
    publish_pair_stats();
    recompute_pass_ranks();   /* studentized ranks + null_flags from valid prefix */

finalize:
    focus_off();
    g_status.paused     = false;
    g_status.elapsed_ms = elapsed_ms_now();
    g_status.state = g_status.abort_requested ? ELOTTO_ABORTED : ELOTTO_DONE;
    vTaskDelete(NULL);
}
