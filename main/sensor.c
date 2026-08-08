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
    if (n > 200000) n = 200000;   /* slave SEG_MAX */
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

static const char *p_label(double absZ)
{
    if (absZ > 3.29) return "p&lt;0.001";
    if (absZ > 2.58) return "p&lt;0.01";
    if (absZ > 1.96) return "p&lt;0.05";
    if (absZ > 1.28) return "p&lt;0.10";
    return "n.s.";
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
/* ── The observer gate (PHASE_READY) ───────────────────────────────────
 * Same one-word handshake as the pool prompt: the webserver task sets the flag,
 * elotto_task spins on it. No timeout, deliberately — there is no sensible
 * default action here (the pool prompt has one: take the proposal), and an
 * unattended session never reaches this gate because it is gated on
 * `pool_confirm`. A session parked here waits as long as the operator needs. */
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
    int nseg = segments_for();               // session window, all phases
    slave_trigger(nseg);
    double zm = 0.0;
    bool   ok = gcp_zscore_ok(nseg, &zm);
    // Sampling is over the moment the local run returns; the reply wait below
    // is dark time. The caller lit the panel — see focus_publish().
    focus_off();
    if (!ok) node_camera_failed(0, "stalled mid-run");
    // Timeout from THIS run's length: at the scoring length the old flat 4 s
    // would expire while every slave was still measuring.
    if (nodes_have_slaves()) nodes_collect(LINK_MEAS_MS_FOR(nseg), true);
    return combine_z(zm, NULL);
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

/* Publish one just-measured item (v3): fold it into the Top-N / Bottom-N
 * lists and refresh the significance line with comparisons = items measured so
 * far. Stored z is RAW and remains independent of every other item. Runs after
 * EVERY item (O(TOP_N)), so /status always shows the live ranking. */
static void publish_result(const RunResult *r, int items_done)
{
    int tn = g_status.result_count;
    if (tn < TOP_N || r->z_score > g_status.top[tn - 1].z_score) {
        if (tn < TOP_N) tn++;
        int p = tn - 1;
        while (p > 0 && g_status.top[p - 1].z_score < r->z_score) {
            g_status.top[p] = g_status.top[p - 1]; p--;
        }
        g_status.top[p] = *r;
        g_status.result_count = tn;   // entries before count, for a racing reader
    }
    int ln = g_status.low_count;
    if (ln < TOP_N || r->z_score < g_status.low[ln - 1].z_score) {
        if (ln < TOP_N) ln++;
        int p = ln - 1;
        while (p > 0 && g_status.low[p - 1].z_score > r->z_score) {
            g_status.low[p] = g_status.low[p - 1]; p--;
        }
        g_status.low[p] = *r;
        g_status.low_count = ln;
    }

    compute_significance(items_done);
}

/* The items closest to the pass mean — the "nothing happened here" group.
 * Contract and the reason it is recomputed rather than accumulated are in
 * sensor.h; this is the plain two-pass implementation of it. */
int results_near_mean(RunResult *out, int cap, double *out_mean, double *out_sigma)
{
    if (out_mean)  *out_mean  = 0.0;
    if (out_sigma) *out_sigma = 0.0;
    if (!out || cap <= 0) return 0;

    int n = g_status.runs_completed;
    if (n > NUM_RUNS) n = NUM_RUNS;
    if (n <= 0) return 0;

    double sum = 0.0;
    for (int j = 0; j < n; j++) sum += g_status.results[j].z_score;
    double mean = sum / n;

    // Sample σ (df = n−1), as everywhere else in this file: the mean it is
    // taken about was estimated from the same data.
    double ss = 0.0;
    for (int j = 0; j < n; j++) {
        double d = g_status.results[j].z_score - mean;
        ss += d * d;
    }
    double sigma = (n > 1 && ss > 0.0) ? sqrt(ss / (n - 1)) : 0.0;

    if (out_mean)  *out_mean  = mean;
    if (out_sigma) *out_sigma = sigma;

    // Insertion sort by |z − mean|, keeping the `cap` smallest. Same shape as
    // publish_result()'s top/low maintenance, one pass, no allocation.
    int got = 0;
    for (int j = 0; j < n; j++) {
        double d = fabs(g_status.results[j].z_score - mean);
        if (got == cap && d >= fabs(out[got - 1].z_score - mean)) continue;
        if (got < cap) got++;
        int p = got - 1;
        while (p > 0 && fabs(out[p - 1].z_score - mean) > d) {
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
    record_loop(m, block_idx);           // before the fold clears the sums
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
    g_status.cal_did_sweep   = false;
    // A new session must not inherit the previous one's calibration age: the
    // rig may have been idle for hours, and the opening insertion is where the
    // operating point has to be established rather than assumed.
    calibrate_forget();
    memset(&s_drift, 0, sizeof(s_drift));
    s_blk_sum = s_blk_sumsq = 0.0;  s_blk_n = 0;
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
    // Defensive only: both pools fit under NUM_RUNS by construction (7920 and
    // 5005), and there is no Runs cap left to shrink this.
    g_status.runs_total = full_combos > NUM_RUNS ? NUM_RUNS : full_combos;

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
     * protocol actually begins. Only when a human asked for it. */
    if (g_status.pool_confirm && !g_status.abort_requested) {
        ready_wait();
        if (g_status.abort_requested) goto done;
        /* Settle. The phase moves off PHASE_READY *first*: the UI re-raises
         * the Start overlay on any /status poll that still sees "ready". */
        g_status.phase = PHASE_SCORING;
        vTaskDelay(pdMS_TO_TICKS(READY_SETTLE_MS));
        g_status.elapsed_ms = elapsed_ms_now();
    }

    /* ── Phase 0: individual number scoring, once per session ────────── */
    g_status.phase = PHASE_SCORING;
    // The WHOLE group's count, up front: 50 main + 12 bonus = 62 for
    // Eurojackpot. Both passes fill one bar that counts to 62.
    g_status.scoring_total = mx + (euro ? 12 : 0);
    g_status.scoring_done  = 0;
    score_and_build_pool(mx, pool_nm, pool_main, false,
                         NULL, NULL, 0, g_status.pool_main_z);
    if (g_status.abort_requested) goto done;
    if (euro) score_and_build_pool(12, POOL_EURO_12, pool_euro, true,
                                   NULL, NULL, 0, g_status.pool_euro_z);
    if (g_status.abort_requested) goto done;
    focus_off();

    /* ── Attended pool confirmation ────────────────────────────────────
     * Stop and show the operator what scoring chose, before ~10 h are spent
     * measuring it. Only when the session asked for it (confirm=1). */
    if (g_status.pool_confirm) {
        pool_confirm_wait(euro, mx, &pool_nm, &pool_ne,
                          pool_main, pool_euro, nm);
        if (g_status.abort_requested) goto done;
        /* The operator may have removed numbers, so recompute everything
         * derived from the pool sizes — at the minimum (pool == draw size)
         * this is exactly ONE combination, measured exactly once. */
        main_combos = comb(pool_nm, nm);
        euro_combos = euro ? comb(pool_ne, 2) : 1;
        full_combos = main_combos * euro_combos;
        g_status.runs_total = full_combos > NUM_RUNS ? NUM_RUNS : full_combos;
    }

    /* ── The pass: every combination once, in random order ─────────────
     * Fisher–Yates over the whole space, once. With a fixed order, slow
     * drift over the ~10 h pass would map onto the enumeration order and
     * read as structure in the numbers; randomized, it spreads evenly.
     * fast_rng() deliberately, not the camera: measurement order is
     * administrative randomness and must not spend camera entropy. */
    for (int i = 0; i < g_status.runs_total; i++) s_perm[i] = (uint16_t)i;
    for (int i = g_status.runs_total - 1; i > 0; i--) {
        int j = (int)(fast_rng() % (uint32_t)(i + 1));
        uint16_t t = s_perm[i]; s_perm[i] = s_perm[j]; s_perm[j] = t;
    }

    int     block          = 0;
    int64_t last_insert_us = esp_timer_get_time();

    g_status.phase = PHASE_MEASURING;
    for (int j = 0; j < g_status.runs_total; j++) {
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

        /* Uncapped and unrepeated, so slot i IS combination i. */
        int i  = s_perm[j];
        int mi = i % main_combos;
        int ei = euro ? (i / main_combos) : 0;
        RunResult *r = &g_status.results[j];      // measurement order
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
        // combine rests on.
        int nseg = segments_for();
        slave_trigger(nseg);
        double zraw = 0.0;
        bool   zok  = gcp_zscore_ok(nseg, &zraw);
        focus_off();          // sampling done; the reply wait is dark time
        if (!zok) node_camera_failed(0, "stalled mid-run");
        if (nodes_have_slaves()) nodes_collect(LINK_MEAS_MS_FOR(nseg), true);

        // Per-node values for the independence check, gathered before the
        // combine so a dropped node is excluded from both consistently.
        // znode[0] is zraw itself: v3 subtracts NOTHING from the master.
        double znode[MAX_NODES] = {0};
        bool   have[MAX_NODES]  = {false};
        double sum = 0.0;
        int    k   = 0;
        if (zok && g_status.nodes[0].ok) {
            znode[0] = zraw; have[0] = true; sum += zraw; k++;
        }
        for (int s = 0; s < nodes_slave_count(); s++) {
            double zs = 0.0;
            if (!g_status.nodes[s + 1].ok || !node_take_z(s, &zs)) continue;
            znode[s + 1] = zs; have[s + 1] = true;
            sum += zs; k++;
        }
        pairs_add_run(znode, have);
        double z = (k > 0) ? sum / sqrt((double)k) : 0.0;

        r->index   = i + 1;
        r->block   = (uint16_t)block;
        r->z_score = z;
        r->chi_sq  = z * z;
        r->p_value = p_label(fabs(z));

        s_blk_sum += z;  s_blk_sumsq += z * z;  s_blk_n++;
        publish_result(r, j + 1);         // top/low + pass stats + Bonferroni
        g_status.runs_completed = j + 1;  // AFTER the row is complete: readers
                                          // (/status, /results.csv) trust the prefix
        g_status.elapsed_ms     = elapsed_ms_now();
        run_gap_ms(gap_for());
    }
    close_block(block);                   // the pass's final block
    goto finalize;

done:
    /* Aborted. results[0..runs_completed) is already compact and already
     * published item by item — nothing to recompute except folding the
     * partial block's pairwise moments so the matrix stays complete. The
     * partial block is deliberately NOT given a /loops row: every stored row
     * describes a full between-insertions span. */
    pairs_fold_loop();
    publish_pair_stats();
    compute_significance(g_status.runs_completed);

finalize:
    focus_off();
    g_status.paused     = false;
    g_status.elapsed_ms = elapsed_ms_now();
    g_status.state = g_status.abort_requested ? ELOTTO_ABORTED : ELOTTO_DONE;
    vTaskDelete(NULL);
}
