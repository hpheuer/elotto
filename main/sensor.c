#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
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

ElottoStatus g_status = {
    .state = ELOTTO_IDLE,
    /* -1, not 0: 0 is a legitimate uptime and would read as "the link
     * dropped at boot" on a board whose link has been up the whole time. */
    .eth_last_down_ms = -1,
    .eth_last_up_ms   = -1,
    .drop_uptime_ms   = -1,
    .drop_node        = -1,
};

/* v3.0 (PLAN.md §2): one pass, every combination exactly once. The cross-loop
 * accumulators (Σz, high/low water marks) are gone with the loops themselves —
 * results[] IS the whole record, raw, in measurement order.
 *
 * Running sums instead, two scopes:
 *  - the BLOCK (between two camera sweeps) feeds the per-block σ,
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

/* Segments per run.
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
 * and the pass all run the same length — and the two "SCORE/MEAS" pairs are
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
 * which also keeps the source out of the starved regime, where camera bias
 * degrades under sustained load.
 *
 * This is the other reason RUN_GAP_MS is not merely cosmetic: the gap is when
 * the producer gets the CPU back, so it buys back most of what it costs.
 *
 * z stays N(0,1) at any run length because it is normalised by √segments; the
 * length only sets granularity, and statistical power per second is
 * rate-limited either way.
 *
 * There is no fixed segment constant here any more: the count is solved from
 * the session's ?run= by segs_from_run_ms() below, against RUN_SEGS_REF /
 * RUN_MS_REF in sensor.h. The old CAM_SEGMENTS 11950 was the 1 s era and had
 * been dead code for several versions. */

/* Segment count for a requested wall window, from RUN_SEGS_REF / RUN_MS_REF in
 * sensor.h — currently the 2026-08-18 pair, 70513 segs ↔ 2703 ms. That header
 * carries the calibration and its history; do not restate the numbers here.
 * Longer requests may stretch past the target: the camera rate falls under
 * sustained load (duty-cycle cliff) — that stretch IS the limit the operator is
 * probing with the ?run= field. */
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

/* Segments for one run. v3: ONE count for every phase — scoring and
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

/* The master's own run, via the shared primitive in components/elotto_gcp — the
 * same object code the slaves run, so no node can compute z differently from
 * another — with the pre-fold channel alongside.
 *
 * NULL yield callback: the master aborts between runs, never inside one, so
 * there is nothing to poll mid-run. A false return means the run produced no
 * usable z and the caller must not score it — a short run is not a small run,
 * its z would be normalised by a √segments it never reached. */
static bool gcp_zscore_ok(int nseg, double *out,
                          bool *out_have_h, double *out_h1, double *out_h2)
{
    double h1 = 0.0, h2 = 0.0;
    bool ok = gcp_zscore_pre(nseg, NULL, out, &h1, &h2) == GCP_OK;
    if (out_have_h) *out_have_h = ok && nseg >= 2;
    if (out_h1) *out_h1 = h1;
    if (out_h2) *out_h2 = h2;
    return ok;
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
static void score_one_run(bool *ok, double *z, double *zcn);
static void score_build_keys(const double *zc, const double *zcc,
                             const bool *scored, int max_val, double *scores);
static void   center_block(int block_idx);  // forward: publish_valid centres the OPEN block

/* Per-item node-z archive (PSRAM). results[j] stays lean; re-analysis needs
 * the per-node series to recombine, drop a soft-failed node, or recompute r. */
static float *s_node_z;   // [NUM_RUNS * MAX_NODES], NaN = did not contribute
/* The PRE-FOLD per-node z archive (D45), same shape and the same NaN
 * convention. It is the channel that keeps what the fold suppresses, so it has
 * to be recoverable per node offline exactly as z0..z3 are. */
static float *s_node_p;
static float *s_node_hw;  /* per-node half-window pre z (D56), NaN = none */
/* Per-node CAMERA sigma of each item's own window (D62), NaN = not reported.
 * ⚠ Not a z and not comparable with the three arrays above: it is the
 * instrument's own noise while that item was measured, the covariate that
 * says whether the z beside it can be trusted at all. */
static float *s_node_wsig;

/* The previous window sigma each node reported, for the jump (D62). NaN
 * until a node has reported once. Reset with the session, not per round:
 * a round boundary is not a discontinuity in the camera. */
static float s_prev_wsig[MAX_NODES];
/* Second moment of every jump this session, for wsig_sd. Not Welford: the mean
 * is zero by construction and a sum of squares over ~1e4 values of order 0,02
 * cannot lose precision in a double. */
static double s_wsig_jsq;
static int    s_wsig_jn;

/* Serialises pass_compact() (elotto_task) against the archive readers on the
 * HTTP task (results_near_mean, results_row_z).
 *
 * A MUTEX, not a spinlock. results_near_mean() walks up to NUM_RUNS rows three
 * times with soft-float arithmetic, i.e. it can hold the lock for milliseconds,
 * and a spinlock (portENTER_CRITICAL) would disable interrupts on that core for
 * the whole walk — lost camera frames and missed UDP reply windows, the very
 * signature that cost the 08-20 session. A mutex only blocks the competing
 * task, so interrupts keep running; the read side is a tight in-memory loop
 * with no blocking call and no nesting, so it cannot deadlock. pass_compact()
 * waits for a reader, which is fine — it runs once per round, not per poll.
 *
 * Created EAGERLY by results_archive_init() (called from app_main), not on
 * first use: a heap failure at the first locked read would otherwise degrade
 * silently to the pre-fix unlocked behaviour. Eager creation turns it into a
 * loud startup error instead. */
static SemaphoreHandle_t s_archive_mutex;

void results_archive_init(void)
{
    if (s_archive_mutex) return;               /* idempotent */
    s_archive_mutex = xSemaphoreCreateMutex();
    if (!s_archive_mutex) {
        /* This is the one lock between compaction and the archive readers. If
         * it cannot be created, every archive_lock() below is a no-op and the
         * pre-fix race returns — so say it, loudly, at boot, not on a poll. */
        printf("sensor: FATAL — archive mutex creation failed; compaction will "
               "race the HTTP readers (no archive synchronisation)\n");
    }
}

static void archive_lock(void)
{
    if (s_archive_mutex) xSemaphoreTake(s_archive_mutex, portMAX_DELAY);
}

static void archive_unlock(void)
{
    if (s_archive_mutex) xSemaphoreGive(s_archive_mutex);
}

static void node_z_store(int j, const double *znode, const bool *have)
{
    if (!s_node_z || j < 0 || j >= NUM_RUNS) return;
    float *row = s_node_z + (size_t)j * MAX_NODES;
    for (int i = 0; i < MAX_NODES; i++)
        row[i] = (have && have[i]) ? (float)znode[i] : NAN;
}

static void node_p_store(int j, const double *zp, const bool *have_p)
{
    if (!s_node_p || j < 0 || j >= NUM_RUNS) return;
    float *row = s_node_p + (size_t)j * MAX_NODES;
    for (int i = 0; i < MAX_NODES; i++)
        row[i] = (have_p && have_p[i]) ? (float)zp[i] : NAN;
}

static void node_hw_store(int j, const double *zhw, const bool *have_hw)
{
    if (!s_node_hw || j < 0 || j >= NUM_RUNS) return;
    float *row = s_node_hw + (size_t)j * MAX_NODES;
    for (int i = 0; i < MAX_NODES; i++)
        row[i] = (have_hw && have_hw[i]) ? (float)zhw[i] : NAN;
}

static void node_wsig_store(int j, const float *wsig)
{
    if (!s_node_wsig || j < 0 || j >= NUM_RUNS) return;
    float *row = s_node_wsig + (size_t)j * MAX_NODES;
    for (int i = 0; i < MAX_NODES; i++)
        row[i] = wsig ? wsig[i] : NAN;   /* already NaN when unreported */
}

/* Move one archive row, for pass_compact(). The source is left as it was: the
 * caller only ever moves DOWN (w < j) and overwrites what it passes on the way,
 * so a stale tail can never be read -- runs_completed is lowered to the new
 * length before anything can look. */
static void node_z_move(int from, int to)
{
    if (from == to) return;
    if (from < 0 || to < 0 || from >= NUM_RUNS || to >= NUM_RUNS) return;
    if (s_node_z)
        memcpy(s_node_z + (size_t)to   * MAX_NODES,
               s_node_z + (size_t)from * MAX_NODES,
               MAX_NODES * sizeof(float));
    /* ⚠ The pre-fold archive moves WITH the z archive, in the same call. Two
     * separate move loops would be two chances to compact one and forget the
     * other, and the result — an item's z paired with a neighbour's pre — is a
     * ranking key that looks entirely plausible. */
    if (s_node_p)
        memcpy(s_node_p + (size_t)to   * MAX_NODES,
               s_node_p + (size_t)from * MAX_NODES,
               MAX_NODES * sizeof(float));
    if (s_node_hw)
        memcpy(s_node_hw + (size_t)to   * MAX_NODES,
               s_node_hw + (size_t)from * MAX_NODES,
               MAX_NODES * sizeof(float));
    if (s_node_wsig)
        memcpy(s_node_wsig + (size_t)to   * MAX_NODES,
               s_node_wsig + (size_t)from * MAX_NODES,
               MAX_NODES * sizeof(float));
}

/* Snapshot one measured item — its RunResult row AND its per-node z — under a
 * single lock. The CSV ?all=1 stream needs both, and reading the row unlocked
 * while pass_compact() rewrites results[] in place could pair a row from the
 * old layout with z-values from the new one. One locked read closes that.
 *
 * Returns false if the archive is missing or j is out of range. out_z gets NaN
 * for nodes that did not contribute that run. Lives in PSRAM so results[]
 * itself stays lean. */
bool results_row_z(int j, RunResult *out_row, float out_z[MAX_NODES],
                   float out_p[MAX_NODES], float out_w[MAX_NODES])
{
    if (!out_row || !out_z || !s_node_z) return false;
    archive_lock();
    if (j < 0 || j >= g_status.runs_completed || j >= NUM_RUNS) {
        archive_unlock();
        return false;
    }
    *out_row = g_status.results[j];
    const float *row = s_node_z + (size_t)j * MAX_NODES;
    for (int i = 0; i < MAX_NODES; i++) out_z[i] = row[i];
    if (out_p) {
        const float *prow = s_node_p ? s_node_p + (size_t)j * MAX_NODES : NULL;
        for (int i = 0; i < MAX_NODES; i++) out_p[i] = prow ? prow[i] : NAN;
    }
    /* The camera sigma of this item's own window (D62), in the SAME locked
     * read: a compaction between two separate reads would pair one item's z
     * with a neighbour's instrument noise, which is exactly the covariate a
     * reader would then trust. */
    if (out_w) {
        const float *wrow = s_node_wsig ? s_node_wsig + (size_t)j * MAX_NODES : NULL;
        for (int i = 0; i < MAX_NODES; i++) out_w[i] = wrow ? wrow[i] : NAN;
    }
    archive_unlock();
    return true;
}

/* Running pass sums over RANKED items only (k > 0, !skip_rank). Ranking and
 * ranks recompute from results[] after every valid item so studentized
 * Top/Bottom track the live mean/σ rather than an early snapshot. */
static double s_pass_sum, s_pass_sumsq;
static int    s_pass_n;

/* Which nodes actually entered the combine during the block center_block() last
 * looked at. Set there because it already walks that block's rows, and read by
 * record_loop() immediately afterwards to decide whether a trip could have
 * contaminated anything. */
static uint8_t s_blk_contrib;

/* How many blocks are FINAL: closed, centred, and their per-node sums folded
 * away, so nothing in them can move again. The open block's index is always
 * exactly this number, since blocks close in order.
 * ⚠ NOT loops_done. That counts /loops rows, and an aborted block is centred
 * without getting one — see the abort path at the `done:` label. Reading
 * loops_done here meant an abort at ?calint=0 ranked nothing at all. */
static int s_blocks_centred;

/* The OPEN block has been centred on its own running per-node means and holds
 * enough items for those means to be worth having (PASS_OPEN_MIN_N).
 *
 * Why this exists: gating the ranking on closed blocks alone left the operator
 * staring at an empty table for a whole cal_interval — 15 minutes by default,
 * and the entire pass at ?calint=0. Not acceptable (user, 2026-08-28), and the
 * fix is not to show uncentred items: raw per-node pre-fold offsets run 20..95
 * sigma, so an uncentred live table ranks WHICH NODES ANSWERED. It is to centre
 * the open block as it fills. The per-node means are then estimated from a
 * handful of items instead of a hundred, i.e. noisier — the tables move as the
 * estimate settles, and again at the block close — but they are estimates of
 * the right thing from the first few items on.
 * ⚠ The items themselves are still provisional. `pass_n_open` in /status counts
 * them and the UI marks the tables, because "provisional" here means the VALUES
 * will change, not that they are meaningless. */
static bool s_open_centred;

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
    /* The operator may only UNCHECK numbers, never add new ones: the selection
     * must stay a subset of the proposal /status is currently showing. A larger
     * one would grow the combination space past what the scoring pass measured
     * -- and, at 15 Eurojackpot main numbers, past NUM_RUNS -- so the pass would
     * be silently truncated into a mislabelled "complete" session. Reject it;
     * the /pool handler turns false into a 409. */
    if (n_main > g_status.pool_n_main || n_euro > g_status.pool_n_euro)
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
    bool   scored[51] = {false};   // this pass produced a usable z (not void)
    /* z and concordance per candidate (D65). Held to the end of the pass
     * because the key needs the pass's own mean and σ per channel. */
    double zc[51], zcc[51];
    for (int i = 0; i < 51; i++) { zc[i] = 0.0; zcc[i] = NAN; }
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
        bool ok = false;
        score_one_run(&ok, &zc[k], &zcc[k]);
        scored[k] = ok;
        g_status.scoring_done++;
        g_status.elapsed_ms = elapsed_ms_now();
        run_gap_ms(gap_for());
    }
    /* Every candidate measured, so the pass's own mean and σ per channel exist:
     * build the keys. Before this point scores[] is empty by construction. */
    score_build_keys(zc, zcc, scored, max_val, scores);

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
    /* Pick by pre-registered score_dir, on the KEY score_build_keys() just
     * produced — not on z. HIGH = largest key (historical default); LOW =
     * smallest; ABS = largest |key|. So at ?wpre=0,8 the direction applies to a
     * pre-fold-dominated quantity, which is the point: the pool is chosen by
     * the same rule the pass is ranked by. Direction is a session parameter
     * (?score=) so the hypothesis is on the record before the pass.
     * Void runs (scored[k] == false) are excluded: a void is not a z of 0 and
     * ranking it as one would steer the pool toward numbers whose runs failed. */
    for (int i = n_keep; i < pool_size; i++) {
        int b = 0;
        double bs = 0.0;
        bool first = true;
        for (int j = 1; j <= max_val; j++) {
            if (used[j] || skip[j] || !scored[j]) continue;
            double s = scores[j], key;
            switch (g_status.score_dir) {
            case SCORE_DIR_LOW: key = -s;           break;
            case SCORE_DIR_ABS: key = fabs(s);      break;
            default:            key = s;            break;  /* HIGH */
            }
            if (first || key > bs) { b = j; bs = key; first = false; }
        }
        /* ⚠ Never write a 0 into the pool. `b` stays 0 only when every
         * remaining candidate voided this pass (scored[] == false). A 0 would
         * then be enumerated as a drawn number and land in the CSV — silent
         * data corruption. Same treatment as the full_combos > NUM_RUNS guard
         * below: abort loudly rather than clamp. */
        if (b == 0) {
            snprintf(g_status.fault, sizeof(g_status.fault),
                     "scoring: only %d of %d candidates produced a usable z "
                     "this pass — session aborted", i - n_keep, pool_size - n_keep);
            printf("pass: %s\n", g_status.fault);
            g_status.abort_requested = true;
            return;
        }
        pool[i] = (uint8_t)b;
        used[b] = true;
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

/* Per-node half-window pre-fold z (D56).
 * Same sign on both halves → √2 · min(|h1|,|h2|) with that sign, which equals
 * the full-window z when the bias is stable across the window. Opposite sign
 * (or a zero half) → 0: a one-sided glitch does not rank. */
static double node_halfwin(double h1, double h2)
{
    if (!((h1 > 0.0 && h2 > 0.0) || (h1 < 0.0 && h2 < 0.0))) return 0.0;
    double m = fabs(h1) < fabs(h2) ? fabs(h1) : fabs(h2);
    return copysign(m, h1) * sqrt(2.0);
}

/* Leave-one-out Stouffer of the per-node half-window z. Drops the loudest
 * node so a single bright-rung offset cannot own the ranking. k < 2 → 0. */
static double conc_stouffer(const double *zhw, const bool *have, int n)
{
    double v[MAX_NODES];
    int k = 0;
    for (int i = 0; i < n && i < MAX_NODES; i++) {
        if (!have[i]) continue;
        v[k++] = zhw[i];
    }
    if (k < 2) return 0.0;
    int imax = 0;
    for (int i = 1; i < k; i++)
        if (fabs(v[i]) > fabs(v[imax])) imax = i;
    double sum = 0.0;
    for (int i = 0; i < k; i++)
        if (i != imax) sum += v[i];
    return sum / sqrt((double)(k - 1));
}

/* Gather per-node z for this round and Stouffer-combine. Soft-downweighted
 * nodes stay in `have[]` (pairwise diagnostics still see them) but are
 * skipped in the combine when that still leaves k ≥ 2; otherwise they are
 * used (never force a solo combine that would collapse the scientific floor).
 * Returns k; k == 0 means VOID — caller must not publish as a result.
 * *out_mask receives the bit mask of nodes that actually entered the combine. */
static int gather_and_combine(double z_master, bool master_ok,
                              double znode[MAX_NODES], bool have[MAX_NODES],
                              double h1[MAX_NODES], double h2[MAX_NODES],
                              bool have_h[MAX_NODES],
                              double h1_master, double h2_master, bool master_h,
                              double *out_z, uint8_t *out_mask)
{
    for (int i = 0; i < MAX_NODES; i++) {
        znode[i]  = 0.0;
        have[i]   = false;
        h1[i] = h2[i] = 0.0;
        have_h[i] = false;
    }
    if (master_ok && g_status.nodes[0].ok) {
        znode[0] = z_master;
        have[0]  = true;
        if (master_h) { h1[0] = h1_master; h2[0] = h2_master; have_h[0] = true; }
    }
    for (int s = 0; s < nodes_slave_count(); s++) {
        double zs = 0.0, a = 0.0, b = 0.0;
        bool   hh = false;
        if (!g_status.nodes[s + 1].ok ||
            !node_take_z(s, &zs, &hh, &a, &b)) continue;
        znode[s + 1] = zs;
        have[s + 1]  = true;
        if (hh) { h1[s + 1] = a; h2[s + 1] = b; have_h[s + 1] = true; }
    }

    /* Soft-exclude only when at least NODE_SOFT_MIN_COMBINE non-soft nodes
     * answered this run.
     *
     * ⚠ "never collapse the array below three arms" stood here and has been
     * false since 2026-08-13: NODE_SOFT_MIN_COMBINE is **1**. A floor of 3 at
     * four nodes permitted exactly ONE exclusion, so when two arms misbehaved
     * the second stayed in and published its offset -- the 08-13 pass is the
     * proof, where slave1 was excluded and the master's block means to -6,33
     * were kept. Up to three of four may now drop out and a SOLO combine is
     * possible; `k` is in the CSV per item so it stays visible afterwards.
     * A bad arm costs more than a small k (user decision). */
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

/* ── One measured window, and the only place a window is measured ──────────
 * Trigger every node, settle, measure locally, collect the replies, combine.
 * Scoring and the pass do exactly this and always did; it lived twice, and the
 * copies drifted. ⚠ That drift is not hypothetical — it is how the pre-fold
 * channel came to rank the pass and not the pool: rank_key() gained a third
 * channel on 2026-08-26 and the scoring copy did not, silently, for two days.
 * A comment saying "keep these in step" had already failed the same way once
 * for the entropy channel. Add a channel HERE and both callers get it.
 *
 * What deliberately stays OUT: the focus panel (only the caller knows whether a
 * number or a draw belongs on screen) and everything archival — results[],
 * blocks, centring, the pairwise matrix. A scoring run is not an item and must
 * never reach the pass statistics.
 *
 * Returns k, and k == 0 means VOID: no usable combine, the caller must not
 * publish it as a result. */
typedef struct {
    double  z, zc;                      // zc 0 when fewer than two arms to corroborate
    double  znode[MAX_NODES], zhw[MAX_NODES];
    bool    have[MAX_NODES], haveh[MAX_NODES];
    uint8_t mask;
    int     k;
} WindowMeas;

static int measure_window(WindowMeas *w)
{
    int nseg = segments_for();
    slave_trigger(nseg);
    bool   fresh = onset_settle();
    double zm = 0.0, h1m = 0.0, h2m = 0.0;
    bool   hok = false;
    bool   zok = fresh && gcp_zscore_ok(nseg, &zm, &hok, &h1m, &h2m);
    focus_off();
    if (fresh && !zok) node_camera_failed(0, "stalled mid-run");
    if (nodes_have_slaves()) nodes_collect(LINK_MEAS_MS_FOR(nseg), true);

    double h1[MAX_NODES], h2[MAX_NODES];
    w->z = 0.0; w->zc = 0.0; w->mask = 0;
    w->k = gather_and_combine(zm, zok, w->znode, w->have,
                              h1, h2, w->haveh,
                              h1m, h2m, hok,
                              &w->z, &w->mask);
    for (int i = 0; i < MAX_NODES; i++) {
        w->zhw[i] = 0.0;
        if (!w->have[i]) continue;
        if (w->haveh[i])
            w->zhw[i] = node_halfwin(h1[i], h2[i]);
        else
            w->zhw[i] = w->znode[i];   /* old node: no halves, full window */
        w->haveh[i] = true;
    }
    bool hw_have[MAX_NODES];
    for (int i = 0; i < MAX_NODES; i++)
        hw_have[i] = w->haveh[i] && (w->mask & (1u << i));
    w->zc = conc_stouffer(w->zhw, hw_have, g_status.node_count);
    return w->k;
}

/* One scoring run, node-combined like Phase 2: trigger every node, measure
 * locally in parallel, combine ÷√k. Both raw channels come back separately —
 * the key is formed by the CALLER, once the whole pass is in.
 *
 * *ok is set to whether the run produced a usable z (k > 0). A void run must
 * NOT be scored as 0.0 and ranked: under score_dir=LOW a 0.0 would beat every
 * positive z, and under HIGH every negative one — so a camera that dropped one
 * run would steer which numbers enter the pool. The caller excludes void
 * candidates from selection instead. */
static void score_one_run(bool *ok, double *z, double *zcn)
{
    WindowMeas m;
    int k = measure_window(&m);
    /* This node's own window log (D64), tag 0 = a scoring run: it has no item
     * to be filed under, and leaving scoring out would put an unexplained gap
     * of ~60 windows at every round boundary in the one time series that is
     * supposed to make gaps impossible to misread. The slaves log their
     * scoring windows too, tagged with the 'M' sequence. */
    camera_winlog_push(0);
    if (ok) *ok = (k > 0);
    *z  = (k > 0) ? m.z  : 0.0;
    if (zcn) *zcn = (k > 0 && m.zc != 0.0) ? m.zc : NAN;
}

/* Scoring key = pass key: folded z plus BOTH pre-fold channels, the pre-fold
 * half split evenly between them (D58) — the same shape rank_key() uses.
 *
 * ⚠ The pass standardises each channel on ITS OWN candidates, because no block
 * mean exists yet: this is the scoring pass, the first measurements of the
 * session (D48). That is what makes the pre-fold channels usable here at all —
 * uncentred they rank NODES, not numbers. */
static void score_build_keys(const double *zc, const double *zcc,
                             const bool *scored, int max_val, double *scores)
{
    /* p = concordance weight, 1-p = z. Same shape as rank_key() (D65). */
    double p = g_status.pre_w;
    double a = 1.0 - p;
    if (a < 0.0) a = 0.0;
    double norm = sqrt(a * a + p * p);
    if (!(norm > 0.0)) norm = 1.0;

    double m[2] = {0}, s[2] = {1.0, 1.0};
    const double *srcs[2] = { zc, zcc };
    for (int ch = 0; ch < 2; ch++) {
        const double *src = srcs[ch];
        if (!src) continue;
        double sum = 0.0, sq = 0.0;
        int    n   = 0;
        for (int k = 1; k <= max_val; k++) {
            if (!scored[k] || isnan(src[k])) continue;
            sum += src[k]; sq += src[k] * src[k]; n++;
        }
        if (n < 2) continue;
        m[ch] = sum / n;
        double v = (sq - n * m[ch] * m[ch]) / (n - 1);
        if (v > 1e-9) s[ch] = sqrt(v);
    }

    for (int k = 1; k <= max_val; k++) {
        if (!scored[k]) { scores[k] = 0.0; continue; }
        double v[2];
        const double raw[2] = { zc[k], zcc ? zcc[k] : NAN };
        for (int ch = 0; ch < 2; ch++) {
            if (isnan(raw[ch])) { v[ch] = 0.0; continue; }
            v[ch] = (raw[ch] - m[ch]) / s[ch];
            if (v[ch] >  ENT_Z_CLAMP) v[ch] =  ENT_Z_CLAMP;
            if (v[ch] < -ENT_Z_CLAMP) v[ch] = -ENT_Z_CLAMP;
        }
        scores[k] = (a * v[0] + p * v[1]) / norm;
    }
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

/* True if this row enters pass mean/σ and Top/Bottom. Void and quarantined
 * trigger-block rows stay in results[] / CSV but not in the ranking. */
/* The value every pass statistic and every ranking runs on: the block-centred
 * combine, not the raw z. One accessor so the choice is made in exactly one
 * place — mixing the two silently would be indistinguishable from a result. */
static double rank_z(const RunResult *r) { return (double)r->z_ctr; }

/* ── The ranking key: folded z and BOTH pre-fold channels, weighted ────────
 *
 * key = ((1−p)·z_ctr + (p/2)·zp_ctr/σ_p + (p/2)·zc_ctr/σ_c)
 *       / √((1−p)² + 2·(p/2)²)
 *
 * p = ?wpre= splits FOLDED against PRE-FOLD; the pre-fold half is then split
 * evenly between the two pre-fold channels (D58, user 2026-08-29):
 *   z_ctr  — combined window z (D65, unfolded).
 *   zc_ctr — concordance (D56): half-window agreement, loudest node dropped.
 * p = ?wpre= is the concordance weight; 1-p is z. p=0 is z alone.
 * ⚠ Each channel is divided by ITS OWN measured σ (rank_sig_p / rank_sig_c).
 * ⚠ Clamped. See ENT_Z_CLAMP — the archive keeps the real value. */
double rank_key(const RunResult *r)
{
    /* p = concordance weight (D65). p=0 is z alone, the control arm. */
    double p = g_status.pre_w;
    if (p <= 0.0) return (double)r->z_ctr;

    double sz = g_status.rank_sig_p;
    if (!(sz > 0.0)) sz = 1.0;
    double sc = g_status.rank_sig_c;
    if (!(sc > 0.0)) sc = 1.0;

    double z = (double)r->z_ctr / sz;
    if (z >  ENT_Z_CLAMP) z =  ENT_Z_CLAMP;
    if (z < -ENT_Z_CLAMP) z = -ENT_Z_CLAMP;

    double zc = (double)r->zc_ctr / sc;
    if (zc >  ENT_Z_CLAMP) zc =  ENT_Z_CLAMP;
    if (zc < -ENT_Z_CLAMP) zc = -ENT_Z_CLAMP;

    double a = 1.0 - p;
    double n = sqrt(a * a + p * p);
    if (!(n > 0.0)) return (double)r->z_ctr;
    return (a * z + p * zc) / n;
}

static bool result_ranked(const RunResult *r)
{
    return r && r->k > 0 && !r->skip_rank;
}

/* Is this item's z_ctr a centred value, i.e. may it enter a statistic?
 *
 * Two ways to qualify and they are not the same thing: a FINAL block (closed,
 * centred, sums folded — its values will not move again), or the OPEN block
 * once it holds enough items to estimate its own per-node means. The open
 * block's index is always s_blocks_centred exactly, because blocks close in
 * order and the counter is bumped at the close.
 *
 * ⚠ This is the whole gate. An item that fails it is not "slightly rough" —
 * uncentred it carries its nodes' raw offsets, which on the pre-fold channel
 * run 20..95 sigma, and one of them in a σ or a Z* scale swamps everything the
 * session actually measured. That is not hypothetical: on hardware 2026-08-27,
 * 45 uncentred items out of 360 drove rank_sig_p to 17,16 against a real 2..4
 * and crushed the top Z* from over 3 to under 1, coming back at the next block
 * close — a sawtooth, not a wrong number sitting still. */
static bool result_centred(const RunResult *r)
{
    int b = (int)r->block;
    return b < s_blocks_centred || (s_open_centred && b == s_blocks_centred);
}

/* ── What compaction left behind ──────────────────────────────────────────
 * Moments of the items pass_compact() dropped, so every pass statistic stays
 * EXACT over the whole session while results[] holds only the survivors.
 *
 * ⚠ This is the load-bearing part of compaction, and the failure mode if it is
 * ever bypassed does not look like a failure. The survivors are the extremes by
 * construction, so a statistic that forgets these sums reports the mean and σ
 * OF THE TABLES: σ ≈ 2 and rising with every compaction, from an instrument
 * whose null gate then fires on its own bookkeeping. Anything that reduces over
 * results[] to describe the SESSION has to add these; anything that reduces to
 * describe the TABLES (top/bottom/nearest, zmax) must not. */
static double s_drop_sum, s_drop_sumsq;
static int    s_drop_n, s_drop_void, s_drop_excl;
/* The same for the two pre-fold channels — needed since rank_key() standardises
 * each by its measured σ. Without this, σ after a compaction would be the σ OF
 * THE SURVIVORS, which are the extremes by construction. Each keeps its own
 * count: an item can be ranked and carry neither, or carry zp_ctr and no
 * zc_ctr (fewer than two nodes survived the leave-one-out drop). */
static double s_drop_psum, s_drop_psumsq;
static int    s_drop_pn;
static double s_drop_csum, s_drop_csumsq;
static int    s_drop_cn;

/* Recompute pass mean/σ/χ² and studentized Top-N / Bottom-N
 * from the ranked prefix. O(n·TOP_N) after each valid item — exact against
 * the live mean, which is the whole point of Z*. */
static void recompute_pass_ranks(void)
{
    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;

    /* Seeded with what compaction dropped, so mean/σ/χ² describe every item the
     * session measured and not just the rows still held.
     *
     * ⚠ CENTRED BLOCKS ONLY, since 2026-08-27 — see result_centred(). rank_z()
     * is z_ctr, and z_ctr is the provisional RAW value until the block it
     * belongs to has been centred (D8) — so an uncentred item carries its
     * nodes' offsets, and a mean or a σ taken over it is measuring the array's
     * own bias, not the null. It showed as a false alarm: a pass σ of 1,171 at
     * 85 items from an instrument that read 1,059 forty items later, with
     * nothing repaired in between. Everything downstream inherits the gate —
     * pass σ, Σz²/n, the valid-item count — which is the point:
     * they all have to describe one set.
     * ⚠ "Centred" INCLUDES the open block once it is self-centred, from
     * PASS_OPEN_MIN_N (4) items on — so the null evidence carries items whose
     * per-node means come from a handful of measurements (SE ~ sigma/2 at the
     * bar) and is measurably unsettled early in every block, tightening as the
     * block fills and shifting once at the close. Accepted for the live tables
     * (user, 2026-08-28); read pass_sigma at a block boundary, not at its
     * start. Only the folded z is affected -- the pre-fold channel ranks but
     * never enters the null (D45).
     * The compaction seeds are safe: pass_compact() runs at a round boundary,
     * i.e. after close_block(), so everything it folded in was centred.
     * ⚠ VOID and EXCLUDED are counted over EVERYTHING. They are archive facts,
     * not statistics, and hiding a void run until its block closes would make
     * the CSV and the live counter disagree. */
    double sum = s_drop_sum, sumsq = s_drop_sumsq;
    int    nv  = s_drop_n, nvoid = s_drop_void, nexcl = s_drop_excl, nopen = 0;
    for (int j = 0; j < ntot; j++) {
        const RunResult *r = &g_status.results[j];
        if (r->k == 0) { nvoid++; continue; }
        if (r->skip_rank) { nexcl++; continue; }
        if (!result_centred(r)) { nopen++; continue; }
        double z = rank_z(r);
        sum += z;
        sumsq += z * z;
        nv++;
    }
    g_status.pass_n_valid = nv;
    g_status.pass_n_open  = nopen;
    g_status.pass_n_void  = nvoid;
    g_status.pass_n_excl  = nexcl;
    g_status.pass_chi2    = sumsq;
    if (nv <= 0) {
        g_status.pass_mean = g_status.pass_sigma = g_status.pass_stouffer = 0.0;
        g_status.result_count = g_status.low_count = 0;
        g_status.comparisons = 0;
        g_status.rank_mean = g_status.rank_sigma = 0.0;
        /* Cleared with the rest: a stale σ from the previous session would
         * standardise the first items of this one against the wrong scale. */
        g_status.rank_sig_p = g_status.rank_sig_c = 0.0;
        g_status.pre_n = g_status.pre_clamped = 0;
        return;
    }
    double mean = sum / (double)nv;
    double ss = sumsq - (double)nv * mean * mean;
    double sigma = (nv > 1 && ss > 0.0) ? sqrt(ss / (double)(nv - 1)) : 0.0;
    g_status.pass_mean     = mean;
    g_status.pass_sigma    = sigma;
    g_status.pass_stouffer = mean * sqrt((double)nv);

    /* ── Two channels, two jobs ────────────────────────────────────────────
     * Everything ABOVE runs on rank_z() and stays there: pass mean/σ/χ² are
     * instrument health on the folded stream.
     *
     * Everything BELOW — the three published tables — runs on rank_key().
     * rank_mean/rank_sigma are its own moments. At ?wpre=0 the key IS z_ctr
     * and the two pairs coincide exactly. */
    /* First: each pre-fold channel's own σ, from the UNCLAMPED archive.
     * rank_key() divides by them, so both have to be settled before the key is
     * called below. Seeded with compaction moments for the same reason as z.
     *
     * ⚠ Two separate accumulators, not one. zp_ctr and zc_ctr are built from
     * the same bits but have different scales — the concordance takes the
     * min() of two halves and drops the loudest node, so its σ runs well below
     * the combined pre-fold σ. Standardising both by one number would hand the
     * key a weight nobody chose (D58). */
    double psum = s_drop_psum, psumsq = s_drop_psumsq;
    int    pn = s_drop_pn;
    double csum = s_drop_csum, csumsq = s_drop_csumsq;
    int    cn = s_drop_cn;
    for (int j = 0; j < ntot; j++) {
        const RunResult *r = &g_status.results[j];
        if (!result_ranked(r)) continue;
        /* ⚠ CENTRED BLOCKS ONLY. An UNCENTRED item still holds the raw value
         * (D8), which carries the per-node offset — on the pre-fold channel
         * that offset is 20..95σ. Letting those into the σ that rank_key()
         * divides by is a feedback loop (sawtooth in Z* on hardware 2026-08-27).
         * The compaction seeds above are safe: pass_compact() runs after
         * close_block(). */
        if (!result_centred(r)) continue;
        if (r->zp_ctr != 0.0f) {
            double pv = (double)r->zp_ctr;
            psum += pv; psumsq += pv * pv; pn++;
        }
        if (r->zc_ctr != 0.0f) {
            double cv = (double)r->zc_ctr;
            csum += cv; csumsq += cv * cv; cn++;
        }
    }
    double pss = psumsq - (pn > 0 ? psum * psum / (double)pn : 0.0);
    g_status.rank_sig_p = (pn > 1 && pss > 0.0) ? sqrt(pss / (double)(pn - 1)) : 0.0;
    double css = csumsq - (cn > 0 ? csum * csum / (double)cn : 0.0);
    g_status.rank_sig_c = (cn > 1 && css > 0.0) ? sqrt(css / (double)(cn - 1)) : 0.0;

    double sp = g_status.rank_sig_p > 0.0 ? g_status.rank_sig_p : 1.0;
    double sc = g_status.rank_sig_c > 0.0 ? g_status.rank_sig_c : 1.0;
    double ksum = 0.0, ksumsq = 0.0;
    /* ⚠ pre_n is SEEDED with what compaction dropped, because the UI prints it
     * against pass_n_valid — and that one is seeded (nv = s_drop_n above). Two
     * counters over two different sets read as an instrument fault: after the
     * first compaction the ratio collapses and the UI's "with pre n/valid ⚠"
     * fires on its own bookkeeping, not on the array (seen 2026-08-30 as
     * "pre 132/788" while every surviving row carried a pre-fold value).
     * ⚠ pre_clamped is NOT seeded and cannot be: whether an item sits at the
     * clamp depends on the σ current at the time of asking, and σ moves. It is
     * a lower bound over the surviving rows after a compaction. */
    int    kn = 0, pre_n = s_drop_pn, pre_clamped = 0;
    for (int j = 0; j < ntot; j++) {
        const RunResult *r = &g_status.results[j];
        if (!result_ranked(r)) continue;
        if (result_centred(r)) {
            double kk = rank_key(r);
            ksum += kk; ksumsq += kk * kk; kn++;
        }
        /* pre_n counts items carrying the pre-fold channel proper. An item can
         * have zp_ctr and no zc_ctr — the leave-one-out drop left k < 2 — so
         * the clamp count is over EITHER channel and counted once per item. */
        if (r->zp_ctr != 0.0f) pre_n++;
        if ((r->zp_ctr != 0.0f && fabs((double)r->zp_ctr) / sp >= ENT_Z_CLAMP) ||
            (r->zc_ctr != 0.0f && fabs((double)r->zc_ctr) / sc >= ENT_Z_CLAMP))
            pre_clamped++;
    }
    double kmean  = (kn > 0) ? ksum / (double)kn : 0.0;
    double kss    = ksumsq - (double)kn * kmean * kmean;
    double ksigma = (kn > 1 && kss > 0.0) ? sqrt(kss / (double)(kn - 1)) : 0.0;
    g_status.rank_mean    = kmean;
    g_status.rank_sigma   = ksigma;
    g_status.pre_n        = pre_n;
    g_status.pre_clamped  = pre_clamped;

    /* Subtracting the mean and dividing by σ is monotone, so it reorders
     * nothing; it only sets the scale the UI prints.
     *
     * Built into LOCAL lists and published at the end: /status and
     * /results.csv read top[]/low[] from the HTTP task, and zeroing the counts
     * before refilling them let a poll land on an empty or half-built table.
     * The counts still move last, so a racing reader sees either the old list
     * or the new one, never a partial one. */
    RunResult ltop[TOP_N], llow[TOP_N];
    int       ltn = 0, lln = 0;

    for (int j = 0; j < ntot; j++) {
        const RunResult *r = &g_status.results[j];
        if (!result_ranked(r)) continue;

        double key = rank_key(r);
        if (ltn < TOP_N || key > rank_key(&ltop[ltn - 1])) {
            if (ltn < TOP_N) ltn++;
            int p = ltn - 1;
            while (p > 0 && rank_key(&ltop[p - 1]) < key) {
                ltop[p] = ltop[p - 1];
                p--;
            }
            ltop[p] = *r;
        }

        if (lln < TOP_N || key < rank_key(&llow[lln - 1])) {
            if (lln < TOP_N) lln++;
            int p = lln - 1;
            while (p > 0 && rank_key(&llow[p - 1]) > key) {
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

    g_status.comparisons = nv;
}

/* ── Round-boundary compaction ─────────────────────────────────────────────
 * Fold everything except the three published tables into moments, so an
 * unlimited session runs until it is aborted instead of stopping at NUM_RUNS.
 *
 * Called ONLY at a round boundary, and only when the next round would not fit.
 * Both halves matter:
 *
 *  - At a round boundary every block of the round has closed, so center_block()
 *    has replaced every provisional z_ctr and the ranking key is FINAL. Doing
 *    this mid-block would rank on values that are about to be rewritten:
 *    replayed against the 2026-08-19 session, a running top-5 on the
 *    provisional values keeps 3 of the true 5, and the item that ends 4th sits
 *    at raw rank 16 when it is measured.
 *  - Only when needed, so a session that fits keeps its complete archive and
 *    behaves exactly as before. Compaction costs rows that can never be
 *    recovered -- the per-node z0..z3 of a dropped item is how the exposure
 *    finding was made -- so it is a last resort, not a policy.
 *
 * Survivors: the PASS_KEEP_EXTREME items with largest |rank_key| (both tails).
 * Kept in MEASUREMENT ORDER, and s_node_z moves with them.
 *
 * ⚠ results[] stops being a complete prefix here. Nothing may infer "the
 * session measured runs_completed items" afterwards; items_done says that. */
static void pass_compact(void)
{
    int n = g_status.runs_completed;
    if (n > NUM_RUNS) n = NUM_RUNS;
    if (n <= 0) return;

    const int K = PASS_KEEP_EXTREME;
    if (n <= K) return;
    uint8_t *keep = heap_caps_calloc((size_t)n, 1, MALLOC_CAP_SPIRAM);
    if (!keep) {                            /* no room to choose: keep everything */
        printf("pass: compaction skipped — no heap for the survivor mask\n");
        return;
    }

    /* Keep the K most extreme by |rank_key|. Linear scan per slot: n is at most
     * NUM_RUNS and K is 100. Both tails survive, so Top-5 and Bottom-5 stay
     * exact over the session. */
    for (int slot = 0; slot < K; slot++) {
        int    best = -1;
        double best_key = 0.0;
        for (int j = 0; j < n; j++) {
            if (keep[j]) continue;
            const RunResult *r = &g_status.results[j];
            if (!result_ranked(r)) continue;
            double key = fabs(rank_key(r));
            if (best < 0 || key > best_key) { best_key = key; best = j; }
        }
        if (best < 0) break;
        keep[best] = 1;
    }

    /* Fold the losers into the moments, then close the gaps in place. Forward
     * copy with w <= j, so the array is rewritten under itself safely and the
     * surviving rows keep their relative order.
     *
     * This whole phase is the critical section: it rewrites results[] and
     * s_node_z in place and then lowers runs_completed, and an HTTP reader
     * (results_near_mean / results_node_z) may be walking either array right
     * now. The selection above only reads, so it needs no lock. */
    archive_lock();
    int w = 0, dropped = 0;
    for (int j = 0; j < n; j++) {
        const RunResult *r = &g_status.results[j];
        if (!keep[j]) {
            if (r->k == 0)          s_drop_void++;
            else if (r->skip_rank)  s_drop_excl++;
            else {
                double z = rank_z(r);
                s_drop_sum   += z;
                s_drop_sumsq += z * z;
                s_drop_n++;
                /* 0 means "no value for this item", the same convention
                 * rank_key() uses — it must not enter a σ. */
                if (r->zp_ctr != 0.0f) {
                    double pv = (double)r->zp_ctr;
                    s_drop_psum += pv; s_drop_psumsq += pv * pv; s_drop_pn++;
                }
                if (r->zc_ctr != 0.0f) {
                    double cv = (double)r->zc_ctr;
                    s_drop_csum += cv; s_drop_csumsq += cv * cv; s_drop_cn++;
                }
            }
            dropped++;
            continue;
        }
        if (w != j) {
            g_status.results[w] = *r;
            node_z_move(j, w);
        }
        w++;
    }
    g_status.runs_completed = w;   /* rows held; items_done is unchanged */
    g_status.compacted     += dropped;
    archive_unlock();

    heap_caps_free(keep);

    printf("pass: compacted at round boundary — %d of %d rows dropped, %d kept "
           "(%d items measured, mean %.6f σ %.6f unchanged)\n",
           dropped, n, w, g_status.items_done, g_status.pass_mean, g_status.pass_sigma);
}

/* Collect this window's camera sigma from every node (D62). The master reads
 * its own camera directly; a slave's arrived on its 'Z' reply and nodes.c left
 * it on the node. NaN throughout means "did not report", which is not zero.
 *
 * ⚠ Called right after measure_window() and before anything can trigger the
 * next run, because cam_wsig_now holds ONE window and the next 'M' overwrites
 * it. */
static void wsig_collect(float out[MAX_NODES])
{
    for (int i = 0; i < MAX_NODES; i++) out[i] = NAN;
    camera_stats_t cs;
    camera_get_stats(&cs);
    if (cs.win_sigma_samples > 0) out[0] = (float)cs.win_sigma;
    for (int i = 1; i < g_status.node_count && i < MAX_NODES; i++)
        out[i] = g_status.nodes[i].cam_wsig_now;
}

/* Offer one item's per-node camera sigmas to the jump board, keeping the
 * WSIG_TOP_N largest |jump| of the whole session (D62).
 *
 * A fixed insertion-sorted array rather than a pass over results[]: results[]
 * is compacted at every round boundary, so a table computed from it would show
 * the current round and nothing else — which is exactly how the rows of a
 * disturbed block came to be unrecoverable on 2026-08-30. The board is the
 * only structure here that outlives compaction, so it copies what it needs.
 *
 * ⚠ One item can put several nodes on the board. That is deliberate: two
 * nodes jumping on the SAME item is a change in the light, one node jumping
 * alone is that camera. Collapsing them to one row per item would erase the
 * distinction that matters most. */
static void wsig_note(const RunResult *r, const float *wsig, uint8_t mask)
{
    for (int i = 0; i < MAX_NODES; i++) {
        float now = wsig[i];
        if (!isfinite(now)) continue;             /* node did not report */
        float prev = s_prev_wsig[i];
        s_prev_wsig[i] = now;                     /* advance regardless */
        if (!isfinite(prev)) continue;            /* first window: no jump yet */

        float jump = now - prev;
        float mag  = jump < 0.0f ? -jump : jump;

        /* The scale FIRST, from every jump including the quiet ones -- that is
         * the whole point of it. Taking it only from the ones that clear the
         * floor would measure the floor instead of the noise. Mean is zero by
         * construction (a stationary camera returns to where it was), so the
         * plain second moment is the spread. */
        s_wsig_jn++;
        s_wsig_jsq += (double)jump * (double)jump;
        g_status.wsig_sd_n = s_wsig_jn;
        g_status.wsig_sd   = (s_wsig_jn > 1) ? sqrt(s_wsig_jsq / (double)s_wsig_jn) : 0.0;

        /* No floor: the board keeps the five largest of whatever happened, and
         * the x-sigma column says whether they matter. See WSIG_TOP_N. */

        /* Insertion sort on |jump|, biggest first. WSIG_TOP_N is 5, so the
         * linear scan is cheaper than anything cleverer and runs once per
         * node per item. */
        int at = g_status.wsig_n;
        for (int j = 0; j < g_status.wsig_n; j++) {
            float e = g_status.wsig_top[j].jump;
            if (e < 0.0f) e = -e;
            if (mag > e) { at = j; break; }
        }
        if (at >= WSIG_TOP_N) continue;           /* smaller than all five */

        for (int j = (g_status.wsig_n < WSIG_TOP_N ? g_status.wsig_n
                                                   : WSIG_TOP_N - 1); j > at; j--)
            g_status.wsig_top[j] = g_status.wsig_top[j - 1];
        if (g_status.wsig_n < WSIG_TOP_N) g_status.wsig_n++;

        WsigEvent *e = &g_status.wsig_top[at];
        e->round   = r->round;
        e->index   = r->index;
        e->node    = (uint8_t)i;
        e->counted = (mask & (1u << i)) ? 1 : 0;
        memcpy(e->nums, r->nums, sizeof(e->nums));
        memcpy(e->euro, r->euro, sizeof(e->euro));
        e->prev = prev;
        e->now  = now;
        e->jump = jump;
    }
}

/* Record WHICH measurements carried a tripping block's spread (D63).
 *
 * Called from record_loop() at the moment the trip fires, which is the only
 * moment the answer exists: the block's rows are still in results[] and their
 * per-node z is still in the archive, and one round later compaction has taken
 * both. `block_idx` is 0-based here and stored 1-based, matching what /loops
 * displays — the two numbering schemes have already cost one wrong reading of a
 * disturbed block.
 *
 * Keeps the TRIPX_TOP_N items furthest from the block mean, by |z - mean|. The
 * archive is the raw per-node z, so this sees exactly what the sigma was
 * computed from. */
static void trip_record(int block_idx, int node, double mean, double sigma)
{
    if (g_status.trip_n >= TRIPX_MAX) return;   /* first trips are the ones worth having */
    if (!s_node_z || node < 0 || node >= MAX_NODES) return;
    if (!(sigma > 0.0)) return;

    TripRec *t = &g_status.trip_hist[g_status.trip_n];
    memset(t, 0, sizeof(*t));
    t->block = (uint16_t)(block_idx + 1);
    t->node  = (uint8_t)node;
    t->sigma = (float)sigma;
    t->mean  = (float)mean;

    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;
    for (int j = 0; j < ntot; j++) {
        const RunResult *r = &g_status.results[j];
        if ((int)r->block != block_idx || r->k == 0) continue;
        float z = s_node_z[(size_t)j * MAX_NODES + node];
        if (!isfinite(z)) continue;                 /* node did not contribute */
        double dev = ((double)z - mean) / sigma;
        double mag = dev < 0.0 ? -dev : dev;

        int at = t->n;
        for (int q = 0; q < t->n; q++) {
            double e = t->it[q].dev;
            if (e < 0.0) e = -e;
            if (mag > e) { at = q; break; }
        }
        if (at >= TRIPX_TOP_N) continue;
        for (int q = (t->n < TRIPX_TOP_N ? t->n : TRIPX_TOP_N - 1); q > at; q--)
            t->it[q] = t->it[q - 1];
        if (t->n < TRIPX_TOP_N) t->n++;

        TripItem *e = &t->it[at];
        e->round = r->round;
        e->index = r->index;
        memcpy(e->nums, r->nums, sizeof(e->nums));
        memcpy(e->euro, r->euro, sizeof(e->euro));
        e->z   = z;
        e->dev = (float)dev;
    }
    if (t->n > 0) {
        g_status.trip_n++;
        printf("trip origin: block %d node %d sigma %.3f — worst item %d/%d at %.2f sigma\n",
               (int)t->block, node, t->sigma, (int)t->it[0].index,
               (int)t->it[0].round, (double)t->it[0].dev);
    }
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

    /* Re-centre the OPEN block on its own running per-node means, so the live
     * tables have something in them from the first few items instead of after a
     * whole cal_interval (user, 2026-08-28). s_nacc already includes this item
     * -- pairs_add_run() ran before we were called -- and center_block()
     * re-derives every z_ctr in the block from the s_node_z archive, so
     * repeating it per item corrects rather than compounds.
     *
     * ⚠ It does NOT mark the block final; only close_block() and the abort path
     * do that. The values here still move, both as the block's means settle and
     * once at the close.
     * ⚠ Cost is one walk of the current block per item, i.e. O(items_in_block),
     * which at a 15-minute block is a few hundred rows of float arithmetic
     * between two ~5 s measurement windows. */
    if (s_blk_n >= PASS_OPEN_MIN_N) {
        center_block((int)r->block);
        s_open_centred = true;
    }
    recompute_pass_ranks();
}

/* Independence diagnostics over every pair of nodes — free bookkeeping that
 * verifies the √n combine assumption. At n=4 there are six pairs, i.e. six ways
 * for the assumption to be false, and ONE correlated pair invalidates the
 * combine. So the worst pair is what gets published; averaging the six would
 * let one bad pair hide behind five good ones.
 *
 * Moments are centered PER LOOP before folding into the session totals: pooling
 * raw values across loops would let each loop's random offset
 * (SE = 1/√n per node) masquerade as correlation.
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
/* Per-node z_h sums over the OPEN block, the entropy twin of s_nacc. Separate
 * counts, not a shared n: a node can answer with a z and no H (older slave
 * firmware, or its FFT buffers did not allocate), so the two channels have
 * different denominators and sharing one would centre the entropy on a mean
 * divided by the wrong count. Cleared with s_nacc by pairs_fold_loop(). */
/* The PRE-FOLD channel's own block accumulator (D45). Its own denominator: a
 * node can answer with a z and no pre-fold value, so k_p is not k and sharing
 * an accumulator would centre on a mean divided by the wrong count. Cleared
 * with the others by pairs_fold_loop(). */
static NodeAcc s_pacc[MAX_NODES];
static NodeAcc s_hwacc[MAX_NODES];

static void pairs_reset(void)
{
    memset(s_pair, 0, sizeof(s_pair));
    memset(s_nacc, 0, sizeof(s_nacc));
    memset(s_pacc, 0, sizeof(s_pacc));
    memset(s_hwacc, 0, sizeof(s_hwacc));
}

/* Fold one run's per-node pre-fold z into the open block. Kept out of
 * pairs_add_run() on purpose: the pairwise independence matrix is a statement
 * about the z channel and the √k combine that rests on it. It needs centring
 * at least as much as z: the raw per-node offset is the exposure rung showing
 * through undamped, which is exactly what the fold used to hide. */
static void pacc_add_run(const double *zp, const bool *have_p)
{
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (!have_p[i]) continue;
        s_pacc[i].s  += zp[i];
        s_pacc[i].ss += zp[i] * zp[i];
        s_pacc[i].n++;
    }
}

static void hwacc_add_run(const double *zhw, const bool *have_hw)
{
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (!have_hw[i]) continue;
        s_hwacc[i].s  += zhw[i];
        s_hwacc[i].ss += zhw[i] * zhw[i];
        s_hwacc[i].n++;
    }
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
        /* ⚠ s_pacc is cleared HERE and nowhere else, in the same loop as
         * s_nacc, because center_block() reads both and this call is what ends
         * their block. It was missing until 2026-08-26 and cost the pre-fold
         * channel outright: mp[] became a cumulative mean over every block, so
         * from the second block on every item was centred on the wrong number. */
        s_pacc[i].s = s_pacc[i].ss = 0.0; s_pacc[i].n = 0;
        s_hwacc[i].s = s_hwacc[i].ss = 0.0; s_hwacc[i].n = 0;

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

    /* The row this block will occupy, or NULL past LOOP_HIST. The soft-down
     * section at the end of this function stamps its verdict onto it; past the
     * cap there is no row and the verdict is printed only. */
    LoopStat *row = NULL;

    camera_stats_t cs;
    camera_get_stats(&cs);
    slaves_diag();                    // nodes are idle between loops

    // Close out this loop's window/gap means before the next loop reopens them.
    float win_ms = 0.0f, gap_ms = 0.0f;
    focus_timing_take(&win_ms, &gap_ms);

    if (g_status.loop_hist && g_status.loop_hist_n < LOOP_HIST) {
        LoopStat *L = &g_status.loop_hist[g_status.loop_hist_n++];
        row = L;                      /* completed once the soft-down verdict is in */
        memset(L, 0, sizeof(*L));
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
            L->die_temp[i] = i ? g_status.nodes[i].die_temp_c : cs.die_temp_c;
            L->cam_exp[i]    = g_status.nodes[i].cam_exp;
            L->cam_gain[i]   = g_status.nodes[i].cam_gain;
            L->cam_fold[i]   = g_status.nodes[i].cam_fold;
            L->cam_cal_ok[i] = g_status.nodes[i].cam_cal_ok;
            L->cam_bias[i]   = g_status.nodes[i].cam_bias;
            /* What the camera did DURING this block, as opposed to what the
             * sweep found. The master reads its own stats directly; the slaves'
             * come from the 'D' reply slaves_diag() collected a few lines up,
             * with the nodes idle between blocks. */
            L->cam_sig[i]  = i ? g_status.nodes[i].cam_sigma_now : (float)cs.sigma;
            L->cam_rsig[i] = i ? g_status.nodes[i].cam_raw_sigma : (float)cs.raw_sigma;
            L->cam_px[i]   = i ? g_status.nodes[i].cam_mean_px
                               : (float)cs.mean_pixel_level;
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
    // whole offset. This IS the drift reference — the separate baseline phase
    // that used to estimate the same number was deleted on 2026-08-28 (D48).
    double raw_off = mean_n[0];
    if (g_status.loops_done == 0) g_status.off_first = raw_off;
    g_status.off_last = raw_off;
    g_status.loops_done++;

    drift_add((double)loop_idx, raw_off);

    /* Soft downweight — sticky, min-k protected, trigger-block quarantine.
     *
     * Trip: σ > NODE_SIGMA_SOFT. σ ONLY — the |mean| wire was removed on
     * 2026-08-19; |mean| over NODE_MEAN_REPORT is flagged into the block row
     * and printed, and excludes nothing. See NODE_MEAN_REPORT in sensor.h.
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
    double trip_bar = 0.0;
    bool   any_trip = false;
    uint8_t mean_mask = 0, trip_mask = 0, soft_mask = 0;

    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        const NodeAcc *a = &s_nacc[i];
        if (a->n < NODE_SOFT_MIN_N) continue;
        double n = (double)a->n, m_i = a->s / n;
        double v = (a->ss - n * m_i * m_i) / (n - 1.0);
        double sig = v > 0.0 ? sqrt(v) : 0.0;
        mean_i[i] = m_i;
        sig_i[i]  = sig;
        have_stats[i] = true;
        if (fabs(m_i) > NODE_MEAN_REPORT) {
            mean_mask |= (uint8_t)(1u << i);
            printf("node %d: block |mean| %.3f over the %.2f report bar "
                   "(σ %.3f, not an exclusion — centred out of the ranking)\n",
                   i, m_i, NODE_MEAN_REPORT, sig);
        }
    }

    /* Trip against THIS block's own median σ (D65). Unfolded σ is not ~1, so
     * an absolute 1,25 bar would fire every block. Two or more arms needed
     * or there is no peer to be loud against. */
    {
        double all_sig[MAX_NODES];
        int nall = 0;
        for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++)
            if (have_stats[i]) all_sig[nall++] = sig_i[i];
        if (nall >= 2) {
            for (int a = 1; a < nall; a++) {
                double vs = all_sig[a];
                int b = a - 1;
                while (b >= 0 && all_sig[b] > vs) { all_sig[b + 1] = all_sig[b]; b--; }
                all_sig[b + 1] = vs;
            }
            double med = (nall & 1) ? all_sig[nall / 2]
                                    : 0.5 * (all_sig[nall / 2 - 1] + all_sig[nall / 2]);
            trip_bar = NODE_SOFT_TRIP_K * med;
            if (trip_bar > 0.0) {
                for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
                    if (!have_stats[i]) continue;
                    if (sig_i[i] > trip_bar) {
                        want[i]  = true;
                        score[i] = sig_i[i] / trip_bar;
                    }
                }
            }
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
    double peer_sig[MAX_NODES];
    int    npeer = 0;
    for (int j = 0; j < g_status.node_count && j < MAX_NODES; j++) {
        if (!g_status.nodes[j].ok || !have_stats[j]) continue;
        if (want[j] || g_status.nodes[j].soft_down) continue;   /* suspect: no reference */
        peer_sig[npeer++] = sig_i[j];
    }
    double clear_sig = NODE_SOFT_CLEAR_SIG;
    if (npeer > 0) {
        for (int a = 1; a < npeer; a++) {          /* insertion sort, n <= 4 */
            double vs = peer_sig[a];
            int b = a - 1;
            while (b >= 0 && peer_sig[b] > vs)  { peer_sig[b + 1]  = peer_sig[b];  b--; }
            peer_sig[b + 1] = vs;
        }
        double med_sig  = (npeer & 1) ? peer_sig[npeer / 2]
                                      : 0.5 * (peer_sig[npeer / 2 - 1] + peer_sig[npeer / 2]);
        double cs = med_sig * NODE_SOFT_CLEAR_SIG_K;
        if (cs > clear_sig)  clear_sig  = cs;      /* floor: never tighter than before */
        /* ...and never as loose as the TRIP bar, or a block that trips the node
         * could also be counted as a clean one. */
        if (trip_bar > 0.0 && clear_sig > trip_bar * NODE_SOFT_CLEAR_MARGIN)
            clear_sig  = trip_bar * NODE_SOFT_CLEAR_MARGIN;
    }
    /* Printed every block because the bar MOVES: without it in the log a
     * clean / not-clean call cannot be checked after the fact. It also travels
     * in the block's /loops row now, so the check survives the console. */
    printf("soft-down clear bar this block: sigma<=%.3f (from %d peer(s))\n",
           clear_sig, npeer);

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
            /* Before anything else touches results[]: this is the only moment
             * the block's own rows still exist (D63). */
            trip_record(loop_idx, i, mean_i[i], sig_i[i]);
            trip_mask |= (uint8_t)(1u << i);
            g_status.nodes[i].soft_down = 1;
            s_soft_clean[i] = 0;
        } else if (g_status.nodes[i].soft_down && have_stats[i]) {
            /* σ only, matching the trip: a criterion that cannot put a node
             * down must not be able to keep it down either. */
            bool clean = (sig_i[i] > 0.0 && sig_i[i] <= clear_sig);
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

    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++)
        if (g_status.nodes[i].soft_down) soft_mask |= (uint8_t)(1u << i);

    if (any_trip) {
        quarantine_block(loop_idx);
        recompute_pass_ranks();
    }

    /* Stamp the verdict onto the row this block already wrote. The LoopStat is
     * filled at the top of record_loop() because it needs the accumulators
     * before they are folded, but the exclusion decision is only reached here —
     * so the row is completed rather than written twice. */
    if (row) {
        row->soft_mask   = soft_mask;
        row->trip_mask   = trip_mask;
        row->mean_mask   = mean_mask;
        row->quarantined = any_trip ? 1 : 0;
        row->clear_sig   = (float)clear_sig;
    }
}


/* Re-derive z_ctr for every item of a block by subtracting each node's own mean
 * over that block, then recombining over the SAME nodes that produced z_score
 * (have_mask, so k is unchanged and the √k scaling still holds). Reads s_nacc,
 * which holds this block's per-node sums until pairs_fold_loop() clears them at
 * the close.
 *
 * ⚠ Called at every block close AND, since 2026-08-28, live during the open
 * block from publish_valid() — s_nacc is a running sum, so the same arithmetic
 * over a partial block is simply a noisier estimate of the same offsets, and it
 * is re-derived from the s_node_z archive every time rather than applied
 * cumulatively. What it does NOT do is decide whether a block counts as
 * centred: that is s_blocks_centred / s_open_centred, set by the callers, so
 * that a live re-centring cannot promote an open block to final.
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

    /* ⚠ The PRE-FOLD channel needs this MOST. The raw per-node bias runs at
     * 1e-3..7e-3 where the folded one is at 1e-5 — the fold was squaring it
     * away, and without the fold it lands in the z at full size. Uncentred,
     * z_pre would rank nodes rather than items.
     * ⚠ And it costs the most: a pre-fold effect CONSTANT across a block is
     * removed with that offset. What survives is variation between items
     * inside one block — which is the pre-registered bargain (D8). */
    double mp[MAX_NODES] = {0};
    bool   okp[MAX_NODES] = {false};
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (s_pacc[i].n >= 2) {
            mp[i]  = s_pacc[i].s / (double)s_pacc[i].n;
            okp[i] = true;
        }
    }
    double mhw[MAX_NODES] = {0};
    bool   okhw[MAX_NODES] = {false};
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (s_hwacc[i].n >= 2) {
            mhw[i]  = s_hwacc[i].s / (double)s_hwacc[i].n;
            okhw[i] = true;
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

        /* The pre-fold channel, its own arms and its own k. */
        const float *prow = s_node_p ? s_node_p + (size_t)j * MAX_NODES : NULL;
        double psum = 0.0;
        int    kp   = 0;
        if (prow) {
            for (int i = 0; i < MAX_NODES; i++) {
                if (!(r->have_mask & (1u << i))) continue;
                if (isnan(prow[i])) continue;
                psum += (double)prow[i] - (okp[i] ? mp[i] : 0.0);
                kp++;
            }
        }
        r->zp_ctr = (kp > 0) ? (float)(psum / sqrt((double)kp)) : 0.0f;

        const float *hwrow = s_node_hw ? s_node_hw + (size_t)j * MAX_NODES : NULL;
        double zhw[MAX_NODES];
        bool   hwh[MAX_NODES];
        for (int i = 0; i < MAX_NODES; i++) {
            zhw[i] = 0.0;
            hwh[i] = false;
            if (!hwrow) continue;
            if (!(r->have_mask & (1u << i))) continue;
            if (isnan(hwrow[i])) continue;
            zhw[i] = (double)hwrow[i] - (okhw[i] ? mhw[i] : 0.0);
            hwh[i] = true;
        }
        r->zc_ctr = (float)conc_stouffer(zhw, hwh, MAX_NODES);
    }
    if (n > 0)
        printf("block %d: centred %d items (node means %+.3f %+.3f %+.3f %+.3f)\n",
               block_idx, n, m[0], m[1], m[2], m[3]);
}

/* ── Block close (v3) ──────────────────────────────────────────────────
 * A block is the span between two camera sweeps. Closing one
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
    /* Final from here on: the sums this block was centred from are about to be
     * folded away, so its z_ctr will not change again. */
    if (block_idx + 1 > s_blocks_centred) s_blocks_centred = block_idx + 1;
    s_open_centred = false;              // the next block starts with nothing in it
    record_loop(m, block_idx);           // before the fold clears the sums
    recompute_pass_ranks();              // centring moved every item in the block
    pairs_fold_loop();
    publish_pair_stats();
    s_blk_sum = s_blk_sumsq = 0.0;
    s_blk_n   = 0;
}

/* ── The session (v3.0, PLAN.md §2): ONE pass, no loops ────────────────
 *
 * calibrate → observer gate → Phase 0 scoring → pool confirm →
 * every combination in the confirmed pool measured EXACTLY ONCE, ~5 s per
 * item, in one Fisher–Yates random order. Every cal_interval_ms (default
 * 15 min) the pass parks for a camera sweep; that boundary
 * closes a block, the unit the drift/pairwise diagnostics run on.
 *
 * results[] fills in MEASUREMENT order (results[j] = j-th item measured, its
 * combination id in .index), so the prefix [0..runs_completed) is always the
 * complete record: publishing, /results.csv and an abort all read it directly,
 * and a single pass never compacts. ⚠ Unlimited mode DOES: pass_compact() runs
 * at a round boundary when the next round would not fit, after which the prefix
 * is the survivors plus s_drop_* moments, not every row measured (D42). */
void elotto_task(void *pvParam)
{
    /* BEFORE the state goes RUNNING, which is the moment /status starts serving
     * the pool: the previous SESSION's numbers must not survive into this one.
     * Everything up to the first scoring pass — discovery, the opening sweep,
     * the observer gate — would otherwise show them, and there is
     * nothing on screen to say they are stale. */
    g_status.pool_n_main     = 0;
    g_status.pool_n_euro     = 0;

    g_status.state           = ELOTTO_RUNNING;
    g_status.runs_completed  = 0;
    g_status.items_done      = 0;
    g_status.compacted       = 0;
    s_drop_sum = s_drop_sumsq = 0.0;
    s_drop_n = s_drop_void = s_drop_excl = 0;
    s_drop_psum = s_drop_psumsq = 0.0;
    s_drop_pn = 0;
    s_drop_csum = s_drop_csumsq = 0.0;
    s_drop_cn = 0;
    // unlimited / runs_cap are session parameters from /start and are NOT reset
    // here — they tag the session, exactly like focus_mode.
    g_status.round           = 0;
    g_status.round_base      = 0;
    g_status.round_item_base = 0;
    g_status.round_start_ms  = 0;
    g_status.round_total     = 0;
    g_status.scoring_done    = 0;
    g_status.abort_requested = false;
    g_status.elapsed_ms      = 0;
    g_status.slave_connected = false;
    g_status.result_count    = 0;
    g_status.low_count       = 0;

    g_status.comparisons     = 0;
    g_status.pass_n_open     = 0;
    g_status.pass_mean       = 0.0;
    g_status.pass_sigma      = 0.0;
    g_status.pass_chi2       = 0.0;
    g_status.pass_stouffer   = 0.0;
    g_status.pass_n_valid    = 0;
    g_status.pass_n_void     = 0;
    g_status.pass_n_excl     = 0;
    g_status.v_eff           = 1.0;
    /* ⚠ Per SESSION, like every counter around it. It is published in /status
     * and in the CSV header, so a single timeout left over from a previous run
     * would be attributed to this one -- for every session that followed, since
     * nothing else ever clears it. The other health counters that are
     * deliberately cumulative (ring_drops, consumer_waits, stalls) live in the
     * camera component and are read as LIFETIME totals; this one describes a
     * session and must not join them. */
    g_status.flush_timeouts  = 0;
    memset(s_soft_clean, 0, sizeof(s_soft_clean));
    g_status.loop_sigma      = 0.0;
    g_status.loops_done      = 0;
    s_blocks_centred         = 0;   /* with loops_done: both count blocks, differently */
    s_open_centred           = false;
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
    /* ~128 KB of PSRAM. If it fails the session runs with folded z alone: a
     * second channel is never a precondition for a measurement. */
    if (!s_node_p)
        s_node_p = heap_caps_malloc((size_t)NUM_RUNS * MAX_NODES * sizeof(float),
                                    MALLOC_CAP_SPIRAM);
    if (s_node_p) {
        for (size_t i = 0; i < (size_t)NUM_RUNS * MAX_NODES; i++)
            s_node_p[i] = NAN;
    }
    if (!s_node_hw)
        s_node_hw = heap_caps_malloc((size_t)NUM_RUNS * MAX_NODES * sizeof(float),
                                     MALLOC_CAP_SPIRAM);
    if (s_node_hw) {
        for (size_t i = 0; i < (size_t)NUM_RUNS * MAX_NODES; i++)
            s_node_hw[i] = NAN;
    }
    /* The camera-sigma archive (D62), same shape and the same NaN convention.
     * A failed allocation costs the CSV columns and the jump board, never a
     * measurement. */
    if (!s_node_wsig)
        s_node_wsig = heap_caps_malloc((size_t)NUM_RUNS * MAX_NODES * sizeof(float),
                                       MALLOC_CAP_SPIRAM);
    if (s_node_wsig) {
        for (size_t i = 0; i < (size_t)NUM_RUNS * MAX_NODES; i++)
            s_node_wsig[i] = NAN;
    }
    /* The board and the per-node history behind it start empty every session:
     * a jump across a reboot or a parameter change is not an event. */
    memset(g_status.trip_hist, 0, sizeof(g_status.trip_hist));
    g_status.trip_n = 0;
    memset(g_status.wsig_top, 0, sizeof(g_status.wsig_top));
    g_status.wsig_n = 0;
    for (int i = 0; i < MAX_NODES; i++) s_prev_wsig[i] = NAN;
    s_wsig_jsq = 0.0; s_wsig_jn = 0;
    g_status.wsig_sd = 0.0; g_status.wsig_sd_n = 0;
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
    /* Per session: the first drop of THIS run is the diagnostic. The eth_*
     * counters above it are lifetime and are deliberately not cleared. */
    g_status.drop_uptime_ms  = -1;
    g_status.drop_node       = -1;
    g_status.drop_eth_up     = false;
    g_status.drop_eth_downs  = 0;
    /* Open a new section in this node's own window log (D64). The slaves do
     * the same off their latch transition, which the discovery below is about
     * to trigger — so all four boundaries land within one command of each
     * other without a wire field to carry them. */
    camera_winlog_new_session();
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

    /* Block index of the pass. Declared HERE, before the first goto done, so
     * the abort path can centre the open block (see done:). */
    int block = 0;

    /* ── Opening insertion: the camera sweep ───────────────────────────
     * Before anything is displayed, so every node has its operating point
     * before the first bit that counts. */
    g_status.cal_did_sweep = calibrate_all();
    if (g_status.abort_requested) { slave_abort(); goto done; }

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
     * so every ranking and the pass mean/σ run on the
     * union of all rounds measured so far, which is what "new results are sorted
     * in" means. Each round is closed as its own block (or blocks), so block
     * centring never mixes items from either side of a re-scoring. */
    int     round          = 0;
    bool    space_full     = false;
    int64_t last_insert_us = esp_timer_get_time();

    for (;;) {
        round++;
        g_status.round = round;
        /* Withdraw the previous round's pool HERE, at the boundary — not further
         * down where the scoring pass starts. The sweep insertion
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
         * = 5005 are both under NUM_RUNS 8000. A pool that still exceeds it is
         * an inflated proposal, so this is a hard stop, NOT a silent clamp:
         * truncating would measure a subset and publish it as complete — the
         * mislabelling this instrument refuses to do. */
        if (full_combos > NUM_RUNS) {
            snprintf(g_status.fault, sizeof(g_status.fault),
                     "combination space %d exceeds NUM_RUNS %d — pool grew past "
                     "its scored proposal; session aborted",
                     full_combos, NUM_RUNS);
            printf("pass: %s\n", g_status.fault);
            g_status.abort_requested = true;
            goto done;
        }

        /* Where this round lands in results[], and how much room is left. The
         * buffer is the hard stop for unlimited mode: a truncated round is still
         * a valid measured prefix, but it is the last one. */
        /* Make room rather than stop. Only when the round would not fit, so a
         * session that never fills the buffer keeps every row it measured and
         * behaves exactly as it did before compaction existed. */
        if (g_status.unlimited && g_status.runs_completed + full_combos > NUM_RUNS)
            pass_compact();

        /* ⚠ runs_completed, NOT items_done. round_base is an INDEX into
         * results[]; items_done is a monotone count of items measured. They
         * were identical until compaction existed and pass_compact() started
         * lowering the one while deliberately leaving the other alone -- after
         * which this round would write past the compacted array, leave the
         * dropped rows sitting in front of it, and count them a second time on
         * top of the moments they were already folded into. That shipped: the
         * 2026-08-20 session reported pass_n_valid 15806 for 8019 items, with
         * every survivor duplicated in top/low/near.
         * The tell is n, not sigma -- doubling identical values barely moves
         * mean or sigma, so the D42 sanity check does not catch this. */
        g_status.round_base      = g_status.runs_completed;
        g_status.round_item_base = g_status.items_done;
        /* Stamped HERE and not at `round++`: the sweep, baseline and scoring
         * pass sit between the two, and none of them measures a combination.
         * Taking the mark at the boundary would put a minute or more of
         * preparation into the round's measuring clock and make the panel's
         * pace figure disagree with its own ETA. */
        g_status.round_start_ms  = (uint32_t)elapsed_ms_now();
        int room = NUM_RUNS - g_status.runs_completed;
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

            /* ── Block boundary: a camera sweep every cal_interval_ms ─────
             * Wall-clock, like the sweep trigger always was — 0 means
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
            // Identical to the scoring window by construction now: both go
            // through measure_window(), which is the point of it.
            WindowMeas w;
            int k = measure_window(&w);
            double  z = w.z;
            uint8_t mask = w.mask;

            /* Archive every node that produced a z (including soft-down ones),
             * so post-hoc recombine is possible. */
            node_z_store(slot, w.znode, w.have);
            node_p_store(slot, w.znode, w.have);  /* same stream (D65) */
            node_hw_store(slot, w.zhw, w.haveh);
            /* The instrument's own noise while THIS item was measured (D62).
             * Read before anything can trigger the next window: cam_wsig_now
             * holds one window and the next 'M' overwrites it. */
            float wsig[MAX_NODES];
            wsig_collect(wsig);
            node_wsig_store(slot, wsig);
            /* The same window into this node's own ring (D64), tagged with the
             * combination id so /camlog lines up against results[] — which
             * compaction will have eaten by the next round boundary. */
            camera_winlog_push((uint32_t)(i + 1));
            pairs_add_run(w.znode, w.have);
            pacc_add_run(w.znode, w.have);
            hwacc_add_run(w.zhw, w.haveh);

            r->index     = i + 1;
            r->block     = (uint16_t)block;
            r->round     = (uint16_t)round;
            r->k         = (uint8_t)k;
            r->have_mask = mask;
            r->skip_rank = 0;   /* quarantine only at block close on soft-down trip */
            /* After index/round/have_mask and after nums/euro above, because
             * the board copies all of them: it has to name the measurement
             * without results[], which compaction will have taken. */
            wsig_note(r, wsig, mask);
            if (k > 0) {
                r->z_score = z;
                /* Provisional: the block's node means are not known until it
                 * closes, so the live ranking uses the uncentred value and
                 * center_block() replaces it a few minutes later. */
                r->z_ctr   = (float)z;
                r->zp_ctr  = (float)z;   /* alias: one stream (D65) */
                r->zc_ctr  = (float)w.zc;
                s_blk_sum += z;  s_blk_sumsq += z * z;  s_blk_n++;
            } else {
                /* VOID: incomplete combine. Archived with k=0, never enters
                 * ranking, pass mean/σ, or block σ. */
                r->z_score = 0.0;
                r->z_ctr   = 0.0f;
                r->zp_ctr  = 0.0f;
                r->zc_ctr  = 0.0f;
            }

            g_status.runs_completed = slot + 1;  // AFTER the row is complete: readers
                                                 // (/status, /results.csv) trust the rows
            g_status.items_done++;               // session progress; compaction never lowers it
            if (k > 0) publish_valid(r);      // studentized top/low + pass health
            g_status.elapsed_ms     = elapsed_ms_now();
            run_gap_ms(gap_for());
        }
        /* The round's final block. Always closed here, even with ?calint=0, so a
         * round is never centred together with the one after it — the pool changed
         * in between, and scoring sat between the two. */
        close_block(block);
        block++;
        if (g_status.unlimited) {
            pass_compact();
            recompute_pass_ranks();
        }

        if (!g_status.unlimited) break;
        if (g_status.abort_requested) break;
        if (space_full || g_status.runs_completed >= NUM_RUNS) {
            snprintf(g_status.fault, sizeof(g_status.fault),
                     "unlimited: results buffer full (%d items) after round %d "
                     "— session ended, pull /results.csv?all=1",
                     g_status.items_done, round);
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
    /* ⚠ Centre the open block FIRST, while s_nacc still holds its per-node
     * sums — pairs_fold_loop() below clears them. Without this, an aborted
     * session ranks the open block on RAW z and every closed block on CENTRED
     * z under one pass mean/σ and Top/Bottom, with no marker to tell the two
     * apart. s_blk_n guards the aborts before the measurement pass (opening
     * sweep, observer gate, scoring), where `block` may not even be
     * initialised and there is nothing to centre. */
    if (s_blk_n > 0) {
        center_block(block);
        /* ⚠ And mark it final, which record_loop() would otherwise have done.
         * The aborted block deliberately gets no /loops row, so loops_done does
         * not move — if the ranking gate read loops_done, an abort at ?calint=0
         * would rank nothing at all over a complete, correctly centred pass. */
        if (block + 1 > s_blocks_centred) s_blocks_centred = block + 1;
        s_open_centred = false;
    }
    pairs_fold_loop();
    publish_pair_stats();
    recompute_pass_ranks();   /* studentized ranks from the valid prefix */

finalize:
    /* Tell the nodes the session is over (D64). slave_abort() is a broadcast
     * 'A' and it was previously sent ONLY on abort_requested — a pass that ran
     * to completion left every slave latched, so /expose and /linearity stayed
     * refused until SESSION_IDLE_MS expired. Sending it here covers both exits;
     * on the abort path it is the second 'A' and costs one datagram, because
     * the slaves' g_abort is reset at the head of every 'M'/'K' anyway. */
    slave_abort();
    focus_off();
    g_status.paused     = false;
    g_status.elapsed_ms = elapsed_ms_now();
    g_status.state = g_status.abort_requested ? ELOTTO_ABORTED : ELOTTO_DONE;
    vTaskDelete(NULL);
}
