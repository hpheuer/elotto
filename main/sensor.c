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
    .k_z   = ZSTAR_KZ_DEFAULT,
    /* -1, not 0: 0 is a legitimate uptime and would read as "the link
     * dropped at boot" on a board whose link has been up the whole time. */
    .eth_last_down_ms = -1,
    .eth_last_up_ms   = -1,
    .drop_uptime_ms   = -1,
    .drop_node        = -1,
};

/* v3: rounds until Abort, every combination once WITHIN a round. The
 * cross-loop accumulators (Σz, high/low water marks) are gone with the loops
 * themselves — results[] IS the whole record, raw, in measurement order.
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
 * from the camera once per session and run forward arithmetically. */
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

/* Segments per run.
 *
 * One window for every focus display: the hold time is not a delay bolted onto
 * a run — it IS the run. The *wire* still carries the count (see
 * slave_trigger), which is what makes the length impossible for a node to get
 * wrong.
 *
 * ⚠ The camera counts are CALIBRATED ON HARDWARE, not derived on paper: the
 * sustained rate is not constant — under load the consumer preempts the capture
 * task it reads from — so re-measure before quoting any figure. RUN_SEGS_REF /
 * RUN_MS_REF in elotto_link.h carry the current calibration. RUN_GAP_MS is not
 * merely cosmetic either: the gap is when the producer gets the CPU back.
 *
 * z stays N(0,1) at any run length because it is normalised by √segments; the
 * length only sets granularity, and statistical power per second is
 * rate-limited either way.
 *
 * There is no fixed segment constant here any more: the count is solved from
 * the session's ?run= by segs_from_run_ms() below, against RUN_SEGS_REF /
 * RUN_MS_REF in elotto_link.h. */

/* Segment count for a requested wall window, from RUN_SEGS_REF / RUN_MS_REF in
 * elotto_link.h — that header carries the calibration; do not restate the numbers
 * here. Longer requests may stretch past the target: the camera rate falls
 * under sustained load, and that stretch IS the limit the operator is probing
 * with the ?run= field. */
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

/* Segments for one run of a phase: the number scoring uses
 * g_status.score_segments behind score_gap_ms (?srun=), the pass
 * g_status.run_segments behind gap_ms (?run=). The count travels on the wire
 * (`M<seg>`) so a slave cannot disagree about it. Defaults are filled in on
 * /start if unset. */
static int segments_for(bool scoring)
{
    if (g_status.run_segments <= 0)
        g_status.run_segments = segs_from_run_ms(
            g_status.run_target_ms > 0 ? g_status.run_target_ms
                                       : RUN_S_DEFAULT * 1000);
    if (g_status.score_segments <= 0)
        g_status.score_segments = segs_from_run_ms(
            g_status.score_target_ms > 0 ? g_status.score_target_ms
                                         : g_status.run_target_ms);
    return scoring ? g_status.score_segments : g_status.run_segments;
}

static int gap_for(bool scoring)
{
    /* /start always resolves both gaps, and 0 is a legal gap. */
    int g = scoring ? g_status.score_gap_ms : g_status.gap_ms;
    return g < 0 ? SCORE_GAP_MS : g;
}

/* The master's own run, via the shared primitive in components/elotto_gcp — the
 * same object code the slaves run, so no node can compute z differently from
 * another. One LSB stream.
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
 * **Maximise the combinations measured**, and nothing else: the chance that a
 * round's pool contains the real draw is (combinations measured)/(combinations
 * that exist) — the main/bonus split cancels out of it entirely — so leaving
 * runs unspent is the ONLY way a pool rule can hurt.
 *
 * 6-of-49 falls out of the same objective: with no second pool, more
 * combinations is more numbers (cap 100 -> 9 numbers, 84 combinations).
 *
 * Eurojackpot's C(p,5)·C(q,2) <= cap does have several splits reaching the same
 * maximum, and there P is identical, so the choice is free — that is where the
 * user's preference for bonus numbers lives: **ties go to the larger bonus
 * pool**, then to the larger main pool. It costs no coverage, which is the
 * whole reason it is a tie-break and not the objective.
 *
 * The minimum (p = draw size, q = 2) is one combination, so any cap >= 1 has a
 * solution. */
/* fix > 0 (?fix=): the first `fix` pool numbers are in every combination, so
 * the main space is C(p−fix, nm−fix) and p may grow to every number. At
 * fix == nm the main space is 1 whatever p is, so p stays nm. */
static void unlimited_pool_sizes(bool euro, int nm, int fix, int cap,
                                 int *out_nm, int *out_ne)
{
    int p_max = fix > 0 ? (euro ? 50 : 49)
                        : (euro ? POOL_MAIN_50 : POOL_MAIN_49);
    if (fix >= nm) p_max = nm;
    int q_min = euro ? 2 : 0;
    int q_max = euro ? POOL_EURO_12 : 0;
    int  best_p = nm, best_q = q_min;
    long best_c = 0;

    for (int p = nm; p <= p_max; p++) {
        long cm = comb(p - fix, nm - fix);
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
// by the pre-registered score_dir (high / low). The pool is locked for
// the whole pass, so this is where selection confidence matters most.
//
// ⚠ Under H₀ the combined z is already N(0,1): its SE is 1.0, not 1/√k.
// Ranking 50/62 unit-noise draws is therefore very noisy. It changes only WHICH
// numbers enter the pool, never the Phase-2 statistics measured on them.
// A session whose pool choice must be trusted on its own wants several full
// random passes; it must NOT go back to repeats in place (onset is the payload).
static void score_one_run(bool *ok, float znode[MAX_NODES],
                          float maun[MAX_NODES], float sern[MAX_NODES], float bytn[MAX_NODES],
                          node_mask_t *mask, ScoreItem *row,
                          float wsig[MAX_NODES]);
static void score_build_keys(const float zn[][MAX_NODES],
                             const node_mask_t *mask,
                             const bool *scored, int max_val,
                             double *zc_out, double *nsd_out);
static double zstar_of(double z, double sz, double nsd, double mau,
                       double ser, double disp, double byt);
static void   center_block(int block_idx);  // forward: publish_valid centres the OPEN block
static void   wsig_collect(float out[MAX_NODES]);
static void   wsig_note(const RunResult *r, const float *wsig, node_mask_t mask,
                        uint8_t spass);
static void   wsig_refresh(bool freeze, int limit);
static void   wsig_fill_scoring(bool euro_pool, uint8_t spass);
static void   archive_lock(void);
static void   archive_unlock(void);
static bool   result_centred(const RunResult *r);
static void   session_limit_check(void);
/* The full table (see full_push()): rows, their Z*, rows in use. */
/* In FULL_CHUNK-row pieces: one 12 MB block need not exist in a PSRAM heap
 * the camera buffers have already cut up. s_full_cap is the rows there is
 * room for (0 = no table), s_full_n the rows in use. */
#define FULL_CHUNK  25000
#define FULL_CHUNKS (FULL_MAX / FULL_CHUNK)
static RunResult *s_full[FULL_CHUNKS];
static float     *s_full_key[FULL_CHUNKS];
static int        s_full_cap;
static int        s_full_n;
#define FULL_ROW(i) (s_full[(i) / FULL_CHUNK][(i) % FULL_CHUNK])
#define FULL_KEY(i) (s_full_key[(i) / FULL_CHUNK][(i) % FULL_CHUNK])
static bool       s_sel_valid;   /* results_extremes() keeps its selection */
static ScoreItem *score_row(bool euro_pool, int k);

/* Per-item node-z archive (PSRAM). results[j] stays lean; centring needs the
 * per-node series to recombine, drop a soft-failed node, or recompute r. */
static float *s_node_z;   // [NUM_RUNS * MAX_NODES], NaN = did not contribute
/* Per-node window σ (,wsig=), NaN = none (Disp). */
static float *s_node_disp;
/* The master's own window σ of the window just measured, NaN = none. */
static float  s_master_wsig = NAN;
/* Per-node window Maurer z, NaN = none (Mau). */
static float *s_node_mau;
/* Per-node serial z of the window, NaN = none (Ser). */
static float *s_node_ser;
/* Per-node byte-frequency z of the window, NaN = none (Byt). */
static float *s_node_byt;
/* Per-ITEM raw synchrony of the window (Sync), NaN = none: Σ of the node-pair
 * slot products over √(their number). One float per results[] row. */
static float *s_item_sync;

/* The previous window sigma each node reported, for the jump. NaN
 * until a node has reported once. Reset with the session, not per round:
 * a round boundary is not a discontinuity in the camera. */
static float s_prev_wsig[MAX_NODES];
/* Second moment of every jump this session, for wsig_sd. Not Welford: the mean
 * is zero by construction and a sum of squares over ~1e4 values of order 0,02
 * cannot lose precision in a double. */
static double s_wsig_jsq;
static int    s_wsig_jn;

/* Serialises pass_compact() (elotto_task) against the archive reader on the
 * HTTP task (results_extremes, behind /extremes).
 *
 * A MUTEX, not a spinlock. A reader walks up to NUM_RUNS rows with soft-float
 * arithmetic, i.e. it can hold the lock for milliseconds, and a spinlock would
 * disable interrupts on that core for the whole walk. A mutex only blocks the
 * competing task, so interrupts keep running; the read side is a tight
 * in-memory loop with no blocking call and no nesting, so it cannot deadlock.
 * pass_compact() waits for a reader, which is fine — it runs once per round,
 * not per poll.
 *
 * Created EAGERLY by results_archive_init() (called from app_main), not on
 * first use: a heap failure at the first locked read would otherwise degrade
 * silently. Eager creation turns it into a loud startup error instead. */
static SemaphoreHandle_t s_archive_mutex;

void results_archive_init(void)
{
    if (s_archive_mutex) return;               /* idempotent */
    s_archive_mutex = xSemaphoreCreateMutex();
    if (!s_archive_mutex) {
        /* This is the one lock between compaction and the archive readers. If
         * it cannot be created, every archive_lock() below is a no-op and the
         * race returns — so say it, loudly, at boot, not on a poll. */
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
    /* ⚠ Every per-node archive moves WITH the z archive, in the same call.
     * Separate move loops would be separate chances to compact one and forget
     * another, and the result — an item's z paired with a neighbour's values —
     * looks entirely plausible. */
    if (s_node_disp)
        memcpy(s_node_disp + (size_t)to   * MAX_NODES,
               s_node_disp + (size_t)from * MAX_NODES,
               MAX_NODES * sizeof(float));
    if (s_node_mau)
        memcpy(s_node_mau + (size_t)to   * MAX_NODES,
               s_node_mau + (size_t)from * MAX_NODES,
               MAX_NODES * sizeof(float));
    if (s_node_ser)
        memcpy(s_node_ser + (size_t)to   * MAX_NODES,
               s_node_ser + (size_t)from * MAX_NODES,
               MAX_NODES * sizeof(float));
    if (s_node_byt)
        memcpy(s_node_byt + (size_t)to   * MAX_NODES,
               s_node_byt + (size_t)from * MAX_NODES,
               MAX_NODES * sizeof(float));
    if (s_item_sync) s_item_sync[to] = s_item_sync[from];
}

static void node_col_store(float *arch, int j, const double *v, const bool *have)
{
    if (!arch || j < 0 || j >= NUM_RUNS) return;
    float *row = arch + (size_t)j * MAX_NODES;
    for (int i = 0; i < MAX_NODES; i++)
        row[i] = (have && have[i]) ? (float)v[i] : NAN;
}

/* A per-node column over one centring span (a block, or a scoring pass):
 * Mau. Item t has the per-node values vals[idx[t]*MAX_NODES + i] and the combine mask mask[t].
 * Each node is centred on its own mean over the span (n >= 2, otherwise it
 * stays out: an uncentred per-camera offset would rank nodes), the rest
 * Stouffer-combined, and the combine divided by its own σ over the span
 * (n >= 3). out[t] = NaN where there is nothing to say. */
static void node_span_col(const float *vals, const int *idx, const node_mask_t *mask,
                     int n, double *out)
{
    double m[MAX_NODES] = {0};
    int    c[MAX_NODES] = {0};
    for (int t = 0; t < n; t++) {
        const float *row = vals + (size_t)idx[t] * MAX_NODES;
        for (int i = 0; i < MAX_NODES; i++)
            if ((mask[t] & (1u << i)) && isfinite((double)row[i])) {
                m[i] += (double)row[i]; c[i]++;
            }
    }
    for (int i = 0; i < MAX_NODES; i++) if (c[i] >= 2) m[i] /= (double)c[i];
    double s = 0.0, ss = 0.0;
    int    ns = 0;
    for (int t = 0; t < n; t++) {
        const float *row = vals + (size_t)idx[t] * MAX_NODES;
        double sum = 0.0;
        int    kk  = 0;
        for (int i = 0; i < MAX_NODES; i++)
            if ((mask[t] & (1u << i)) && c[i] >= 2 && isfinite((double)row[i])) {
                sum += (double)row[i] - m[i]; kk++;
            }
        out[t] = kk ? sum / sqrt((double)kk) : NAN;
        if (kk) { s += out[t]; ss += out[t] * out[t]; ns++; }
    }
    double sd = 0.0;
    if (ns >= 3) {
        double mu = s / ns, v = (ss - ns * mu * mu) / (ns - 1);
        if (v > 0.0) sd = sqrt(v);
    }
    for (int t = 0; t < n; t++)
        out[t] = (sd > 0.0 && isfinite(out[t])) ? out[t] / sd : NAN;
}

/* Running pass sums over RANKED items only (k > 0, !skip_rank). Ranking and
 * ranks recompute from results[] after every valid item so top[] tracks
 * the live block-σ key rather than an early snapshot. */
static double s_pass_sum, s_pass_sumsq;
static int    s_pass_n;

/* Which nodes actually entered the combine during the block center_block() last
 * looked at. Set there because it already walks that block's rows, and read by
 * record_loop() immediately afterwards to decide whether a trip could have
 * contaminated anything. */
static node_mask_t s_blk_contrib;

/* How many blocks are FINAL: closed, centred, and their per-node sums
 * cleared, so nothing in them can move again. The open block's index is always
 * exactly this number, since blocks close in order.
 * ⚠ NOT loops_done. That counts /loops rows, and an aborted block is centred
 * without getting one — see the abort path at the `done:` label. Reading
 * loops_done here meant an abort before the first round closed ranked
 * nothing at all. */
static int s_blocks_centred;

/* The OPEN block has been centred on its own running per-node means and holds
 * enough items for those means to be worth having (PASS_OPEN_MIN_N).
 *
 * Uncentred items must not be shown: raw per-node LSB offsets run 20..95
 * sigma, so an uncentred live table ranks WHICH NODES ANSWERED. Centring the
 * open block as it fills estimates the means from a handful of items instead of
 * a hundred — noisier, so the tables move as the estimate settles and again at
 * the block close — but they are estimates of the right thing from the first
 * few items on.
 * ⚠ The items themselves are still provisional. `pass_n_open` in /status counts
 * them and the UI marks the tables, because "provisional" here means the VALUES
 * will change, not that they are meaningless. */
static bool s_open_centred;

/* Per-block ranking σ. Indexed by results[].block. Frozen at close_block
 * so compaction can drop the rows and rank_key() still divides by the σ of the
 * WHOLE block, not of the surviving extremes. The open block is recomputed
 * from the live rows each time the tables refresh. PSRAM: BSIG_MAX × 12 B.
 * BSIG_MAX is every block a 72 h session can close (the shortest legal round
 * is ~60 runs), far past the LOOP_HIST rows /loops keeps — a row of the full
 * table must still find its σ.
 * ⚠ NOT loops_done / loop_hist: an aborted block is centred and ranked without
 * a /loops row, and still needs a σ. */
typedef struct {
    float    sig_p;    // sample σ of z_ctr over the block; 0 = unknown
    uint8_t  frozen;   // 1 = closed (or aborted-and-centred); do not recompute
} BlockRankSig;
#define BSIG_MAX 8192
static BlockRankSig *s_bsig;

/* Consecutive clean blocks while a node is soft_down (sticky clear). */
static uint8_t s_soft_clean[MAX_NODES];

/* Every measurement window starts on fresh bits: the flush drops the ring and
 * leaves the accumulators alone.
 *
 * Why it is needed: nothing consumes the ring during the gap, so it is FULL
 * when a window opens — bits produced before the item existed. Those bits are
 * not bad, they are MISLABELLED, and this rig treats crediting bits to the
 * wrong combination as the error that matters.
 *
 * Returns false if the flush did not complete inside ONSET_SETTLE_MS. The
 * caller MUST then produce no measurement.
 *
 * ⚠ A timed-out flush is worse than a late one. camera_ring_flush() drops the
 * ring at the next PAIR BOUNDARY, so if no pair arrives the ring is never
 * dropped at all — the run would then consume exactly the stale bits the flush
 * existed to remove, and nothing downstream could tell.
 *
 * VOID is the house rule: gcp_zscore_raw() refuses to return a short run rather
 * than normalise it by a √segments it never reached. A run whose provenance is
 * unknown is treated the same way — archived with k=0, never ranked. It is NOT
 * a camera fault: a pair costs 56 ms idle and 85 ms loaded, so ONSET_SETTLE_MS
 * (two pairs, one discarded) is a long stall but well short of
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

/* ── The pool's sums  ──────────────────────────────────────────────
 * One accumulator per pool (0 = main numbers, 1 = bonus numbers) and per
 * column (ScoreSum), over the closed passes of this round's scoring. The pool
 * is picked from ONE of them — g_status.score_sum as it stands when the whole
 * scoring ends — so every column is summed all the way through and switching
 * costs nothing. Independent of the display rows, which may fail to allocate. */
typedef struct {
    double acc[SCORE_SUM_N][51];
    bool   ok[SCORE_SUM_N][51];
    uint32_t n[SCORE_SUM_N][51];  /* passes that added to acc; 0 = column missing.
                                   * Never saturates: score_rank_val() divides by it. */
    int    max_val, pool_size;
    bool   active;
} ScoreAcc;
static ScoreAcc s_sacc[2];
static int      s_live_sum = -1;   /* the column the live pool was last built on */

static void score_acc_begin(int ix, int max_val, int pool_size)
{
    memset(&s_sacc[ix], 0, sizeof(s_sacc[ix]));
    s_sacc[ix].max_val   = max_val;
    s_sacc[ix].pool_size = pool_size;
    s_sacc[ix].active    = true;
}

/* Column name for logs and faults, ASCII like the rest of the Log card. */
static const char *score_sum_label(int ch)
{
    static const char *const L[SCORE_SUM_N] = { "Z*", "Z", "Disp", "dn", "Ser", "Mau", "Byt" };
    return (ch >= 0 && ch < SCORE_SUM_N) ? L[ch] : "?";
}

static int score_sum_sel(void)
{
    int c = g_status.score_sum;
    return (c >= 0 && c < SCORE_SUM_N) ? c : SUM_KEY;
}

/* What a number is ranked on: its sum over the n passes that added to it, per
 * pass — Σ/√n for the columns read against 0, Σ/n for Δn (read against 1).
 * ⚠ Not the bare sum: a number that lacks a pass has a shorter sum, which
 * under Δn alone is a full σ of that sum. With the same n for every number
 * this is the order of the sums. NaN = nothing added. */
static double score_rank_val(const ScoreAcc *A, int ch, int j)
{
    double n = (double)A->n[ch][j];
    if (!(n > 0.0)) return NAN;
    return A->acc[ch][j] / (ch == SUM_NSD ? n : sqrt(n));
}

static int score_pick_best(const ScoreAcc *A, int ch, const bool *used)
{
    int b = 0;
    double bs = 0.0;
    bool first = true;
    /* Σ Z* ranks by magnitude: high = largest |sum|, low = smallest.
     * acc stays signed, and pool_*_z prints that sign. Equal |sum| keeps
     * the lower number (strict >). Other columns stay signed. */
    bool mag = (ch == SUM_KEY);
    for (int j = 1; j <= A->max_val; j++) {
        if (used[j] || !A->ok[ch][j]) continue;
        double s = score_rank_val(A, ch, j);
        if (!isfinite(s)) continue;
        if (mag) s = fabs(s);
        double key = g_status.score_dir == SCORE_DIR_LOW ? -s : s;
        if (first || key > bs) { b = j; bs = key; first = false; }
    }
    return b;
}

/* The pool as it would be picked now, on the selected column, into /status —
 * after every pass and whenever the operator switches the column. */
static void score_publish_live(int ix)
{
    const ScoreAcc *A = &s_sacc[ix];
    if (!A->active) return;
    int ch = score_sum_sel();
    bool used[51] = {false};
    int n = 0;
    for (int i = 0; i < A->pool_size; i++) {
        int b = score_pick_best(A, ch, used);
        if (!b) break;
        used[b] = true;
        if (ix) {
            g_status.pool_euro[n]   = (uint8_t)b;
            g_status.pool_euro_z[n] = (float)A->acc[ch][b];
        } else {
            g_status.pool_main[n]   = (uint8_t)b;
            g_status.pool_main_z[n] = (float)A->acc[ch][b];
        }
        n++;
    }
    if (ix) g_status.pool_n_euro = (uint8_t)n;
    else    g_status.pool_n_main = (uint8_t)n;
    s_live_sum = ch;
}

/* The final pick, on the column selected NOW — called once the whole scoring
 * (both runs in Eurojackpot) is over. Sorted ascending for a stable
 * combination enumeration; out_z follows the sort. */
/* `fix` > 0: the first `fix` picks (the best) are the fixed numbers; the
 * fixed part and the free part are each sorted on their own, fixed first. */
static void pool_sort(uint8_t *pool, int n)
{
    for (int i = 1; i < n; i++) {
        uint8_t key = pool[i]; int j = i - 1;
        while (j >= 0 && pool[j] > key) { pool[j+1] = pool[j]; j--; }
        pool[j+1] = key;
    }
}

static bool score_pick_pool(int ix, uint8_t *pool, float *out_z, int fix)
{
    const ScoreAcc *A = &s_sacc[ix];
    int ch = score_sum_sel();
    bool used[51] = {false};
    for (int i = 0; i < A->pool_size; i++) {
        int b = score_pick_best(A, ch, used);
        if (b == 0) {
            snprintf(g_status.fault, sizeof(g_status.fault),
                     "scoring: only %d of %d candidates carry a %s sum "
                     "— session aborted", i, A->pool_size, score_sum_label(ch));
            printf("pass: %s\n", g_status.fault);
            g_status.abort_requested = true;
            return false;
        }
        pool[i] = (uint8_t)b;
        used[b] = true;
    }
    if (fix < 0 || fix > A->pool_size) fix = 0;
    pool_sort(pool, fix);
    pool_sort(pool + fix, A->pool_size - fix);
    if (out_z)
        for (int i = 0; i < A->pool_size; i++)
            out_z[i] = (float)A->acc[ch][pool[i]];
    return true;
}

/* TEST ?fakepool=1: n distinct numbers of 1..max_val, sorted ascending. */
static void fake_pick_pool(int max_val, int n, uint8_t *pool, float *out_z)
{
    uint8_t v[51];
    for (int i = 0; i < max_val; i++) v[i] = (uint8_t)(i + 1);
    for (int i = 0; i < n; i++) {
        int j = i + (int)(fast_rng() % (uint32_t)(max_val - i));
        uint8_t t = v[i]; v[i] = v[j]; v[j] = t;
    }
    for (int i = 1; i < n; i++) {
        uint8_t key = v[i]; int j = i - 1;
        while (j >= 0 && v[j] > key) { v[j+1] = v[j]; j--; }
        v[j+1] = key;
    }
    for (int i = 0; i < n; i++) { pool[i] = v[i]; if (out_z) out_z[i] = 0.0f; }
}

/* One Log line for the whole pool: main and bonus numbers are picked on the
 * same criterion. */
static void pool_log(const uint8_t *pm, int nm, const uint8_t *pe, int ne,
                     int fix)
{
    char nums[200];
    int  np = 0;
    nums[0] = '\0';
    if (fix > 0)
        np += snprintf(nums + np, sizeof(nums) - np, " fixed");
    for (int i = 0; i < nm && np < (int)sizeof(nums) - 4; i++) {
        if (fix > 0 && i == fix)
            np += snprintf(nums + np, sizeof(nums) - np, " | free");
        np += snprintf(nums + np, sizeof(nums) - np, " %d", pm[i]);
    }
    if (ne > 0 && np < (int)sizeof(nums) - 4)
        np += snprintf(nums + np, sizeof(nums) - np, " +");
    for (int i = 0; i < ne && np < (int)sizeof(nums) - 4; i++)
        np += snprintf(nums + np, sizeof(nums) - np, " %d", pe[i]);
    if (score_sum_sel() == SUM_KEY)
        evlog("Pool by %s |sum| of Z*:%s",
              g_status.score_dir == SCORE_DIR_LOW ? "smallest" : "largest", nums);
    else
        evlog("Pool by %s sum of %s:%s",
              g_status.score_dir == SCORE_DIR_LOW ? "lowest" : "highest",
              score_sum_label(score_sum_sel()), nums);
}

/* ── The scoring table (ScoreItem) ────────────────────────────────────────
 * Main numbers first, bonus numbers after them — `s_score_main_n` is where the
 * bonus rows start. Display only: nothing here feeds the pool. */
static int s_score_main_n;

static void score_rows_begin(int main_max, bool euro)
{
    s_score_main_n = main_max;
    ScoreItem *R = g_status.score_rows;
    if (!R) { g_status.score_rows_n = 0; return; }
    int n = main_max + (euro ? 12 : 0);
    if (n > SCORE_ROWS_MAX) n = SCORE_ROWS_MAX;
    memset(R, 0, (size_t)n * sizeof(ScoreItem));
    for (int i = 0; i < n; i++) {
        ScoreItem *s = &R[i];
        bool e = (i >= main_max);
        int  num = e ? i - main_max + 1 : i + 1;
        s->r.index = num;
        s->r.round = (uint16_t)g_status.round;
        if (e) s->r.euro[0] = (uint8_t)num;
        else   s->r.nums[0] = (uint8_t)num;
        s->r.node_sd = NAN;
        run_ser_set(&s->r, NAN);
        run_byt_set(&s->r, NAN);
        s->r.sync_c  = RUN_SYNC_NONE;   /* pass items only */
        s->r.mau     = NAN;
        s->key       = NAN;
    }
    g_status.score_sig_z = 0.0;
    g_status.score_span_n = 0;
    g_status.score_rows_n = n;
}

static ScoreItem *score_row(bool euro_pool, int k)
{
    int i = euro_pool ? s_score_main_n + k - 1 : k - 1;
    if (!g_status.score_rows || i < 0 || i >= g_status.score_rows_n) return NULL;
    return &g_status.score_rows[i];
}

/* ── Scoring-sum archive ───────────────────────────────────────────────
 * One snap per score_run, main and euro separately. The live rows and
 * s_sacc are cleared at the next round; this is what Save reads after Abort.
 * Cleared at the next session start, not on abort. Earliest snaps stay —
 * a later scoring past the cap is counted in s_snap_drop. */
static ScoreSnap     *s_snaps;
static volatile int   s_snap_n;
static volatile int   s_snap_drop;

_Static_assert(SCORE_PASSES_MAX <= 255, "ScoreSnap.passes is one byte");
_Static_assert(sizeof(ScoreSnap) <= 2048, "ScoreSnap");

static void score_snap_reset(void)
{
    if (!s_snaps) {
        s_snaps = heap_caps_calloc(SCORE_SNAP_MAX, sizeof(ScoreSnap), MALLOC_CAP_SPIRAM);
        if (!s_snaps)
            printf("score snap alloc failed (%u bytes)\n",
                   (unsigned)(SCORE_SNAP_MAX * sizeof(ScoreSnap)));
    }
    s_snap_n = 0;
    s_snap_drop = 0;
    __sync_synchronize();
}

/* passes = closed passes already added to A. 0 stores nothing. */
static void score_snap_store(const ScoreAcc *A, bool euro, int passes)
{
    if (!A || passes <= 0) return;
    if (!s_snaps) { s_snap_drop++; return; }
    int n = s_snap_n;
    if (n >= SCORE_SNAP_MAX) { s_snap_drop++; return; }
    int mv = A->max_val;
    if (mv < 1) return;
    if (mv > 50) mv = 50;
    if (passes > 255) passes = 255;
    ScoreSnap *S = &s_snaps[n];
    S->round  = (uint16_t)g_status.round;
    S->euro   = euro ? 1 : 0;
    S->nnum   = (uint8_t)mv;
    S->passes = (uint8_t)passes;
    for (int k = 1; k <= mv; k++) {
        for (int c = 0; c < SCORE_SUM_N; c++) {
            S->cnt[k][c] = (uint8_t)(A->n[c][k] > 255 ? 255 : A->n[c][k]);
            S->sum[k][c] = A->n[c][k] ? (float)A->acc[c][k] : 0.0f;
        }
    }
    __sync_synchronize();
    s_snap_n = n + 1;
}

/* Scoring-only accumulate mode (maxruns=0, passes=0) is ONE growing scoring,
 * not a series: keep a single snap per pool (main → slot 0, euro → slot 1),
 * refreshed each pass, so Save and /extremes?sums=1 read the current totals
 * rather than 128 per-pass snaps that then drop. Non-accumulate appends as
 * before. */
static void score_snap_put(const ScoreAcc *A, bool euro, int passes, bool accumulate)
{
    if (!accumulate) { score_snap_store(A, euro, passes); return; }
    if (!A || passes <= 0 || !s_snaps) return;
    int mv = A->max_val;
    if (mv < 1) return;
    if (mv > 50) mv = 50;
    if (passes > 255) passes = 255;
    int slot = euro ? 1 : 0;
    ScoreSnap *S = &s_snaps[slot];
    S->round  = (uint16_t)g_status.round;
    S->euro   = euro ? 1 : 0;
    S->nnum   = (uint8_t)mv;
    S->passes = (uint8_t)passes;
    for (int k = 1; k <= mv; k++) {
        for (int c = 0; c < SCORE_SUM_N; c++) {
            S->cnt[k][c] = (uint8_t)(A->n[c][k] > 255 ? 255 : A->n[c][k]);
            S->sum[k][c] = A->n[c][k] ? (float)A->acc[c][k] : 0.0f;
        }
    }
    __sync_synchronize();
    if (s_snap_n < slot + 1) s_snap_n = slot + 1;
}

int score_snap_count(void)   { return s_snap_n; }
int score_snap_dropped(void) { return s_snap_drop; }

const ScoreSnap *score_snap_at(int i)
{
    int n = s_snap_n;
    __sync_synchronize();
    if (!s_snaps || i < 0 || i >= n) return NULL;
    return &s_snaps[i];
}

// `euro_pool` selects the bonus-number pool; it only reaches the Focus panel,
// which styles a euro candidate differently from a main one.
/* One scoring run: every number `passes` times, every column summed into
 * s_sacc[euro_pool]. It picks nothing — score_pick_pool() does, once the whole
 * scoring is over — and it no longer snaps: the caller stores the snap so
 * accumulate mode (maxruns=0, passes=0) can keep ONE growing snap instead of
 * one per pass. `reset` false keeps the accumulator across calls (accumulate);
 * `pass_base` is added to the live pass counter so a continuous scoring shows
 * its cumulative pass number. Returns the passes closed into A this call. */
static int score_run(int max_val, int pool_size, bool euro_pool,
                     int passes, bool reset, int pass_base)
{
    const int ix = euro_pool ? 1 : 0;
    if (reset) score_acc_begin(ix, max_val, pool_size);
    ScoreAcc *A = &s_sacc[ix];
    /* Per-node archive for ONE scoring pass. Function-static: ~1,6 KB. */
    static float zn[51][MAX_NODES];
    static float sg[51][MAX_NODES];   /* window σ per node (Disp) */
    static float mn[51][MAX_NODES];   /* window Maurer z per node (Mau) */
    static float sn[51][MAX_NODES];   /* window serial z per node (Ser) */
    static float bn[51][MAX_NODES];   /* window byte-frequency z per node (Byt) */
    node_mask_t smask[51];
    /* The health line describes THIS scoring run's closed passes only — the
     * euro-number run must not show the main run's last span as its own. */
    g_status.score_sig_z = 0.0;
    g_status.score_span_n = 0;

    uint8_t order[51];
    int     n_order = 0;
    for (int i = 1; i <= max_val; i++)
        order[n_order++] = (uint8_t)i;
    /* scoring_total is set by the caller to scoring_passes × (main+bonus). */

    uint8_t last = 0;
    int closed = 0;   /* passes whose sums are already in A */
    g_status.scoring_euro = euro_pool ? 1 : 0;
    for (int pass = 0; pass < passes; pass++) {
        g_status.scoring_pass = pass_base + pass + 1;
        bool scored[51] = {false};
        for (int i = 0; i < 51; i++) {
            smask[i] = 0;
            for (int n = 0; n < MAX_NODES; n++)
                zn[i][n] = sg[i][n] = mn[i][n] = sn[i][n] = bn[i][n] = NAN;
        }
        for (int i = n_order - 1; i > 0; i--) {
            int j = (int)(fast_rng() % (uint32_t)(i + 1));
            uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
        }
        if (n_order > 1 && last && order[0] == last) {
            uint8_t t = order[0]; order[0] = order[1]; order[1] = t;
        }
        for (int idx = 0; idx < n_order; idx++) {
            session_limit_check();
            if (g_status.abort_requested) goto snap;
            pause_gate();
            if (g_status.abort_requested) goto snap;
            int k = order[idx];
            focus_show_number(k, euro_pool);
            bool ok = false;
            float wsig[MAX_NODES];
            score_one_run(&ok, zn[k], mn[k], sn[k], bn[k], &smask[k],
                          score_row(euro_pool, k), wsig);
            for (int n = 0; n < MAX_NODES; n++) sg[k][n] = wsig[n];
            /* The jump board sees every window, scoring included: a camera
             * that moves during the hour of scoring is the same camera that
             * then measures the pass. Named by number and pass, not item. */
            {
                ScoreItem *row = score_row(euro_pool, k);
                RunResult id;
                memset(&id, 0, sizeof(id));
                id.round = (uint16_t)g_status.round;
                id.index = k;
                if (euro_pool) id.euro[0] = (uint8_t)k;
                else           id.nums[0] = (uint8_t)k;
                wsig_note(row ? &row->r : &id, wsig, smask[k], (uint8_t)(pass + 1));
            }
            scored[k] = ok;
            g_status.scoring_done++;
            /* The operator switched the column: show the pool it picks now. */
            if (score_sum_sel() != s_live_sum) {
                score_publish_live(0);
                score_publish_live(1);
            }
            g_status.elapsed_ms = elapsed_ms_now();
            run_gap_ms(gap_for(true));
        }
        last = n_order ? order[n_order - 1] : 0;
        double zc_ctr[51], nsd[51];
        score_build_keys(zn, smask, scored, max_val, zc_ctr, nsd);
        /* Mau, Ser, Disp and Byt over this pass: the pass is its own centring span. */
        double maup[51], serp[51], dispp[51], bytp[51];
        {
            int     idx[51], nb = 0;
            node_mask_t bm[51];
            double  bo[51];
            for (int k = 0; k < 51; k++) maup[k] = serp[k] = dispp[k] = bytp[k] = NAN;
            for (int k = 1; k <= max_val; k++)
                if (scored[k]) { idx[nb] = k; bm[nb] = smask[k]; nb++; }
            node_span_col(&mn[0][0], idx, bm, nb, bo);
            for (int t = 0; t < nb; t++) maup[idx[t]] = bo[t];
            node_span_col(&sn[0][0], idx, bm, nb, bo);
            for (int t = 0; t < nb; t++) serp[idx[t]] = bo[t];
            node_span_col(&sg[0][0], idx, bm, nb, bo);
            for (int t = 0; t < nb; t++) dispp[idx[t]] = bo[t];
            node_span_col(&bn[0][0], idx, bm, nb, bo);
            for (int t = 0; t < nb; t++) bytp[idx[t]] = bo[t];
        }
        for (int k = 1; k <= max_val; k++) {
            if (!scored[k]) continue;
            ScoreItem *row = score_row(euro_pool, k);
            /* This pass's value per column. A missing one adds nothing. */
            double v[SCORE_SUM_N];
            double key = zstar_of(zc_ctr[k], g_status.score_sig_z, nsd[k], maup[k],
                                  serp[k], dispp[k], bytp[k]);
            v[SUM_KEY]  = key;
            v[SUM_Z]    = zc_ctr[k];
            v[SUM_DISP] = dispp[k];
            v[SUM_NSD]  = nsd[k];
            v[SUM_SER]  = serp[k];
            v[SUM_MAU]  = maup[k];
            v[SUM_BYT]  = bytp[k];
            for (int c = 0; c < SCORE_SUM_N; c++) {
                if (!isfinite(v[c])) continue;
                A->acc[c][k] += v[c];
                A->ok[c][k]   = true;
                A->n[c][k]++;
            }
            if (!row) continue;
            /* The pass closed: centred values, as center_block() writes an
             * item's. */
            row->r.z_ctr   = isnan(zc_ctr[k]) ? 0.0f : (float)zc_ctr[k];
            row->r.disp    = (float)dispp[k];
            row->r.node_sd = (float)nsd[k];
            row->r.mau     = (float)maup[k];
            run_ser_set(&row->r, serp[k]);
            run_byt_set(&row->r, bytp[k]);
            row->key       = (float)key;
            for (int c = 0; c < SCORE_SUM_N; c++) {
                row->sums[c] = (float)A->acc[c][k];
                /* Saturate: continuous scoring (accumulate) runs far past 255
                 * passes, and a wrap would read as "0 closed passes". */
                if (isfinite(v[c]) && row->sum_n[c] < 255) row->sum_n[c]++;
            }
            if (row->passes < 255) row->passes++;
        }
        closed++;
        wsig_fill_scoring(euro_pool, (uint8_t)(pass + 1));
        score_publish_live(ix);

        /* ── Sweep candidate after every scoring pass   ────────
         * So the pool is not chosen on a single operating point held for the
         * whole scoring run. Whether it actually sweeps is the dynamic
         * interval's call (calibrate_all()).
         *
         * Why at a pass boundary and not anywhere else in the loop: score_build_keys() has
         * just run, so this pass is centred and scaled and closed. Every pass
         * is its own centring span, which makes a pass boundary the only place
         * a rung may move without shifting a node's offset underneath the mean
         * that is being subtracted from it — the same argument as the sweep
         * between the scoring and the pass.
         *
         * ⚠ calibrate_all() leaves g_status.phase at PHASE_CALIBRATE — it is the
         * caller's job to restore it, and here the scoring is not over. */
        /*: every closed scoring pass is a candidate; calibrate_all()
         * sweeps only once the dynamic interval has run out. */
        if (pass + 1 < passes) {
            if (calibrate_all(euro_pool ? "scoring pass, euro numbers" : "scoring pass"))
                g_status.cal_did_sweep = true;
            if (g_status.abort_requested) goto snap;
            g_status.phase = PHASE_SCORING;
        }
    }
snap:
    return closed;
}

/* Gather per-node z for this run and Stouffer-combine. Soft-downweighted
 * nodes stay in `have[]` (pairwise diagnostics still see them) but are
 * skipped in the combine when that still leaves k ≥ NODE_SOFT_MIN_COMBINE
 * (1 — a solo combine is possible). Returns k; k == 0 means VOID — caller
 * must not publish as a result.
 * *out_mask receives the bit mask of nodes that actually entered the combine. */
static int gather_and_combine(double z_master, bool master_ok,
                              double znode[MAX_NODES], bool have[MAX_NODES],
                              double *out_z, node_mask_t *out_mask)
{
    for (int i = 0; i < MAX_NODES; i++) {
        znode[i]  = 0.0;
        have[i]   = false;
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
     * answered this run.
     *
     * ⚠ NODE_SOFT_MIN_COMBINE is **1**. Up to three of four may drop out and a
     * SOLO combine is possible; `k` is kept per item in results[]. A bad arm
     * costs more than a small k. */
    int n_soft = 0;
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (have[i] && !g_status.nodes[i].soft_down) n_soft++;
    }
    bool use_soft = (n_soft >= NODE_SOFT_MIN_COMBINE);

    double sum = 0.0;
    int    k   = 0;
    node_mask_t mask = 0;
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (!have[i]) continue;
        if (use_soft && g_status.nodes[i].soft_down) continue;
        sum += znode[i];
        k++;
        mask |= (node_mask_t)(1u << i);
    }
    if (out_mask) *out_mask = mask;
    if (out_z) *out_z = (k > 0) ? sum / sqrt((double)k) : 0.0;
    return k;
}

/* ── One measured window, and the only place a window is measured ──────────
 * Trigger every node, settle, measure locally, collect the replies, combine.
 * ⚠ Add a channel HERE and both callers get it.
 *
 * What deliberately stays OUT: the focus panel (only the caller knows whether a
 * number or a draw belongs on screen) and everything archival — results[],
 * blocks, centring, the pairwise matrix. A scoring run is not an item and must
 * never reach the pass statistics.
 *
 * Returns k, and k == 0 means VOID: no usable combine, the caller must not
 * publish it as a result. */
typedef struct {
    double  z;
    double  znode[MAX_NODES];
    double  mau[MAX_NODES];             /* window Maurer z, raw (Mau) */
    double  ser[MAX_NODES];             /* window serial z, raw (Ser) */
    double  byt[MAX_NODES];             /* window byte-frequency z, raw (Byt) */
    bool    have[MAX_NODES], havemau[MAX_NODES], haveser[MAX_NODES], havebyt[MAX_NODES];
    node_mask_t mask;
    int     k;
    double  sync;                       /* window synchrony, raw (Sync); NaN = none */
} WindowMeas;

/* The cameras' synchrony inside one window: per time slot the products of
 * every two combined nodes' slot z, (Σz)² − Σz² over 2, summed over the slots
 * and divided by √(number of products) — mean 0, about unit variance for
 * independent cameras. Products only, never a node's own square: a camera's
 * own dispersion is Disp and must not read as agreement. NaN without a pair. */
static double window_sync(float sl[][EL_SLOTS], node_mask_t mask)
{
    double c = 0.0;
    int    np = 0;
    for (int t = 0; t < EL_SLOTS; t++) {
        double s = 0.0, ss = 0.0;
        int    k = 0;
        for (int i = 0; i < MAX_NODES; i++) {
            if (!(mask & (1u << i)) || !isfinite((double)sl[i][t])) continue;
            s += (double)sl[i][t]; ss += (double)sl[i][t] * (double)sl[i][t]; k++;
        }
        if (k < 2) continue;
        c  += 0.5 * (s * s - ss);
        np += k * (k - 1) / 2;
    }
    return np > 0 ? c / sqrt((double)np) : NAN;
}

static int measure_window(WindowMeas *w, bool scoring)
{
    int nseg = segments_for(scoring);
    if (scoring) g_status.win_n_score++; else g_status.win_n_pass++;
    slave_trigger(nseg);
    bool   fresh = onset_settle();
    double zm = 0.0;
    bool   zok = fresh && gcp_zscore_ok(nseg, &zm);
    /* The master's own window Maurer z, from the run just finished. */
    double maum = zok ? gcp_last_maurer() : NAN;
    double serm = zok ? gcp_last_serial() : NAN;
    double bytm = zok ? gcp_last_bytechi() : NAN;
    /* The master's own window σ NOW, before waiting for the slaves: its camera
     * keeps adding mini-runs to the window statistics until the next flush, so
     * read later it would span more bits than the z above, by an amount that
     * follows the network. A slave reads its own when it replies. */
    {
        camera_stats_t cs;
        camera_get_stats(&cs);
        s_master_wsig = (zok && cs.win_sigma_samples > 0) ? (float)cs.win_sigma : NAN;
    }
    /* The master's own time slots, at the same instant and for the same reason. */
    static float sl[MAX_NODES][EL_SLOTS];
    for (int i = 0; i < MAX_NODES; i++)
        for (int t = 0; t < EL_SLOTS; t++) sl[i][t] = NAN;
    if (zok) camera_win_slots(el_slot_us(nseg), sl[0], EL_SLOTS);
    focus_off();
    if (fresh && !zok) node_camera_failed(0, "stalled mid-run");
    if (nodes_have_slaves()) nodes_collect(LINK_MEAS_MS_FOR(nseg), true);

    w->z = 0.0; w->mask = 0;
    w->k = gather_and_combine(zm, zok, w->znode, w->have, &w->z, &w->mask);
    for (int i = 1; i < MAX_NODES && i < g_status.node_count; i++)
        if (w->have[i]) node_slots(i - 1, sl[i]);
    w->sync = window_sync(sl, w->mask);
    for (int i = 0; i < MAX_NODES; i++) {
        double a = (i == 0) ? maum
                 : (i < g_status.node_count ? (double)g_status.nodes[i].cam_mau_now : NAN);
        w->havemau[i] = w->have[i] && isfinite(a);
        w->mau[i]     = w->havemau[i] ? a : 0.0;
    }
    for (int i = 0; i < MAX_NODES; i++) {
        double a = (i == 0) ? serm
                 : (i < g_status.node_count ? (double)g_status.nodes[i].cam_ser_now : NAN);
        w->haveser[i] = w->have[i] && isfinite(a);
        w->ser[i]     = w->haveser[i] ? a : 0.0;
    }
    for (int i = 0; i < MAX_NODES; i++) {
        double a = (i == 0) ? bytm
                 : (i < g_status.node_count ? (double)g_status.nodes[i].cam_byt_now : NAN);
        w->havebyt[i] = w->have[i] && isfinite(a);
        w->byt[i]     = w->havebyt[i] ? a : 0.0;
    }
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
static void score_one_run(bool *ok, float znode[MAX_NODES],
                          float maun[MAX_NODES], float sern[MAX_NODES], float bytn[MAX_NODES],
                          node_mask_t *mask, ScoreItem *row,
                          float wsig[MAX_NODES])
{
    WindowMeas m;
    int k = measure_window(&m, true);
    /* Before the next 'M' overwrites them — same rule as the pass. */
    wsig_collect(wsig);
    if (row) {
        /* The number's latest measurement, filled as the pass fills an item
         *: raw z and provisional z_ctr = z until
         * its pass closes, no key and no Δn yet. A VOID is k = 0 and zeros. */
        RunResult *r = &row->r;
        r->k         = (uint8_t)k;
        r->have_mask = (k > 0) ? m.mask : 0;
        r->round     = (uint16_t)g_status.round;
        r->z_score   = (k > 0) ? m.z : 0.0;
        r->z_ctr     = (k > 0) ? (float)m.z  : 0.0f;
        r->disp      = NAN;   /* set when the pass closes */
        run_ser_set(r, NAN);  /* set when the pass closes */
        run_byt_set(r, NAN);  /* likewise */
        r->sync_c    = RUN_SYNC_NONE;   /* pass items only */
        r->mau       = NAN;   /* set when the pass closes */
        r->node_sd   = NAN;
        row->key     = NAN;
    }
    /* The node table's Z column is the session mean of every raw per-run z,
     * scoring included. Only that display mean: the pairwise matrix and the
     * block accumulators stay pass-only (a scoring run is not an item). */
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (!m.have[i]) continue;
        NodeStatus *N = &g_status.nodes[i];
        N->z_n++;
        N->z_mean += (m.znode[i] - N->z_mean) / (double)N->z_n;
    }
    /* This node's own window log, tag 0 = a scoring run: it has no item
     * to be filed under, and leaving scoring out would put an unexplained gap
     * of ~60 windows at every round boundary in the one time series that is
     * supposed to make gaps impossible to misread. The slaves log their
     * scoring windows too, tagged with the 'M' sequence. */
    camera_winlog_push(0, gcp_last_maurer());
    if (ok) *ok = (k > 0);
    if (mask) *mask = (k > 0) ? m.mask : 0;
    for (int i = 0; i < MAX_NODES; i++) {
        znode[i] = m.have[i]  ? (float)m.znode[i] : NAN;
        maun[i]  = m.havemau[i] ? (float)m.mau[i] : NAN;
        sern[i]  = m.haveser[i] ? (float)m.ser[i] : NAN;
        bytn[i]  = m.havebyt[i] ? (float)m.byt[i] : NAN;
    }
}

/* Centre one scoring pass and publish its σ. The product Z* is applied by
 * the caller once Mau of this pass exists (zstar_of).
 *
 * ⚠ There is no /loops block here — a scoring run is not an item. The span
 * of this function is the block: each node's mean is over the numbers it
 * actually answered. Uncentred LSB ranks NODES; dropping the loudest
 * RAW node before that centre dropped the camera on the bright rung, not
 * the one with the real number-to-number jump. */
static void score_build_keys(const float zn[][MAX_NODES],
                             const node_mask_t *mask,
                             const bool *scored, int max_val,
                             double *zc_out, double *nsd_out)
{
    /* Per-node means over the span. */
    double mz[MAX_NODES] = {0};
    int    nz[MAX_NODES] = {0};
    bool   okz[MAX_NODES] = {false};
    for (int k = 1; k <= max_val; k++) {
        if (!scored[k]) continue;
        for (int i = 0; i < MAX_NODES && i < g_status.node_count; i++) {
            if (!isnan((double)zn[k][i])) { mz[i] += (double)zn[k][i]; nz[i]++; }
        }
    }
    for (int i = 0; i < MAX_NODES; i++) {
        if (nz[i] >= 2) { mz[i] /= (double)nz[i]; okz[i] = true; }
    }
    /* Per-node σ over the span: it scales each node in the combine and in Δn.
     * n >= 3 for the same reason as in center_block(): at 2 the σ is built
     * from the two values it scales. */
    double sdz[MAX_NODES] = {0};
    bool   sdok[MAX_NODES] = {false};
    {
        double vs[MAX_NODES] = {0};
        for (int k = 1; k <= max_val; k++) {
            if (!scored[k]) continue;
            for (int i = 0; i < MAX_NODES && i < g_status.node_count; i++)
                if (okz[i] && !isnan((double)zn[k][i])) {
                    double d = (double)zn[k][i] - mz[i];
                    vs[i] += d * d;
                }
        }
        for (int i = 0; i < MAX_NODES; i++) {
            if (!okz[i] || nz[i] < 3) continue;
            double v = vs[i] / (double)(nz[i] - 1);
            if (v > 0.0) { sdz[i] = sqrt(v); sdok[i] = true; }
        }
    }
    if (nsd_out)
        for (int k = 0; k <= max_val && k < 51; k++) nsd_out[k] = NAN;

    double zc[51];
    for (int k = 0; k <= max_val && k < 51; k++) zc[k] = NAN;
    for (int k = 1; k <= max_val; k++) {
        if (!scored[k]) continue;
        double sum = 0.0, rsum = 0.0, usum = 0.0;
        int    kk  = 0, rk = 0, uk = 0;
        for (int i = 0; i < MAX_NODES; i++) {
            if (!(mask[k] & (1u << i))) continue;
            if (isnan((double)zn[k][i])) continue;
            rsum += (double)zn[k][i];
            rk++;
            /* ⚠ A node with fewer than two values in this pass stays out of
             * the combine, as in center_block(): raw, its offset would be
             * this number's z and would widen the span σ. */
            if (!okz[i]) continue;
            sum += (double)zn[k][i] - mz[i];
            kk++;
            if (sdok[i]) { usum += ((double)zn[k][i] - mz[i]) / sdz[i]; uk++; }
        }
        /* Each node in its own σ over this pass, as in center_block(). Without
         * any σ: centred, unscaled. No centrable node at all (a pass of one
         * scored number): the raw combine stands; that span has no σ, so its
         * Z* is 0. */
        zc[k]  = (uk > 0) ? usum / sqrt((double)uk)
               : (kk > 0) ? sum / sqrt((double)kk)
               : (rk > 0) ? rsum / sqrt((double)rk) : NAN;
        if (nsd_out) {
            /* Δn, as center_block() forms it for an item: each node's centred z
             * in that node's own σ over this span, sample σ across the nodes. */
            double u[MAX_NODES];
            int    ku = 0;
            for (int i = 0; i < MAX_NODES; i++) {
                if (!(mask[k] & (1u << i)) || !sdok[i]) continue;
                if (isnan((double)zn[k][i])) continue;
                u[ku++] = ((double)zn[k][i] - mz[i]) / sdz[i];
            }
            double v = NAN;
            if (ku >= 2) {
                double um = 0.0;
                for (int t = 0; t < ku; t++) um += u[t];
                um /= (double)ku;
                double vv = 0.0;
                for (int t = 0; t < ku; t++) vv += (u[t] - um) * (u[t] - um);
                vv /= (double)(ku - 1);
                v = vv > 0.0 ? sqrt(vv) : 0.0;
            }
            nsd_out[k] = v;
        }
    }
    if (zc_out)   for (int k = 0; k <= max_val && k < 51; k++) zc_out[k]   = zc[k];

    /* σ of this scoring span. Fewer than two finite values, or a span whose
     * variance is ~0, publishes 0 and Z* is 0. No fallback of 1. */
    double sum = 0.0, sq = 0.0, sd = 0.0;
    int    n   = 0;
    for (int k = 1; k <= max_val; k++) {
        if (!scored[k] || isnan(zc[k])) continue;
        sum += zc[k]; sq += zc[k] * zc[k]; n++;
    }
    if (n >= 2) {
        double mean = sum / n;
        double v = (sq - n * mean * mean) / (n - 1);
        if (v > 1e-9) sd = sqrt(v);
    }
    g_status.score_sig_z  = sd;
    g_status.score_span_n = n;
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

/* True if this row enters pass mean/σ and the ranking tables. Void and quarantined
 * trigger-block rows stay in results[] but not in the ranking. */
/* The value every pass statistic and every ranking runs on: the block-centred
 * combine, not the raw z. One accessor so the choice is made in exactly one
 * place — mixing the two silently would be indistinguishable from a result. */
static double rank_z(const RunResult *r) { return (double)r->z_ctr; }

static bool result_ranked(const RunResult *r)
{
    return r && r->k > 0 && !r->skip_rank;
}

/* Is this item's z_ctr a centred value, i.e. may it enter a statistic?
 *
 * Two ways to qualify and they are not the same thing: a FINAL block (closed,
 * centred, sums — its values will not move again), or the OPEN block
 * once it holds enough items to estimate its own per-node means. The open
 * block's index is always s_blocks_centred exactly, because blocks close in
 * order and the counter is bumped at the close.
 *
 * ⚠ This is the whole gate. An item that fails it is not "slightly rough" —
 * uncentred it carries its nodes' raw offsets, which on the LSB channel
 * run 20..95 sigma, and one of them in a σ or a Z* scale swamps everything the
 * session actually measured. */
static bool result_centred(const RunResult *r)
{
    int b = (int)r->block;
    return b < s_blocks_centred || (s_open_centred && b == s_blocks_centred);
}

/* σ of z_ctr over one block's centred items. Frozen at close so
 * compaction can drop the rows. skip_rank items still enter: the σ describes
 * the instrument of that span, not the ranking set. */
static void block_sig_compute(int block_idx, bool freeze)
{
    if (block_idx < 0 || block_idx >= BSIG_MAX || !s_bsig) return;
    BlockRankSig *B = &s_bsig[block_idx];
    if (B->frozen && !freeze) return;

    double psum = 0.0, psq = 0.0;
    int    pn = 0;
    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;
    for (int j = 0; j < ntot; j++) {
        const RunResult *r = &g_status.results[j];
        if ((int)r->block != block_idx || r->k == 0) continue;
        if (!result_centred(r)) continue;
        double z = rank_z(r);
        psum += z; psq += z * z; pn++;
    }
    double pss = psq - (pn > 0 ? psum * psum / (double)pn : 0.0);
    B->sig_p = (pn > 1 && pss > 0.0) ? (float)sqrt(pss / (double)(pn - 1)) : 0.0f;
    if (freeze) B->frozen = 1;
}

static void block_sig_refresh_open(void)
{
    if (!s_open_centred) return;
    int b = s_blocks_centred;
    if (b < 0 || b >= BSIG_MAX || !s_bsig) return;
    if (s_bsig[b].frozen) return;
    block_sig_compute(b, false);
}

static double block_sig_of(const RunResult *r)
{
    int b = r ? (int)r->block : -1;
    return (s_bsig && b >= 0 && b < BSIG_MAX) ? (double)s_bsig[b].sig_p : 0.0;
}

/* Weight as used by Z*: NaN and anything outside (0, 1] becomes 0 (off)
 * or 1 (full). The /start parser already refuses values outside 0..1. */
static double zstar_w01(double w)
{
    if (!(w > 0.0)) return 0.0;
    return w > 1.0 ? 1.0 : w;
}

/* Z* = sign(Z)·|Z/σ_z|^kz · [1 + ζ·|Disp|] · [1 + α·(1/Δn − 1)]
 *              · [1 + ε·|Ser|] · [1 + β·|Mau|] · [1 + η·|Byt|]
 * (written in the Top-10 column order; the product is commutative)
 *
 * α β ε ζ η are g_status.k_dn / k_mau / k_ser / k_disp / k_byt, each 0..1.
 * Mau, Ser, Disp and Byt stretch by their MAGNITUDE: either sign is the cameras
 * departing together from the rest of the block.
 * A missing column (NaN, Δn ≤ 0) leaves
 * its factor at 1. No σ_z → 0, never a fallback of 1.
 * ⛔ UNBOUNDED. Z/σ_z sits inside its block, the product of the factors
 * does not: a small Δn or a large |Mau|, |Ser| or |Disp| can exceed (n−1)/√n.
 * Nothing here truncates an extreme. */
static double zstar_of(double z, double sz, double nsd, double mau,
                       double ser, double disp, double byt)
{
    if (!(sz > 0.0) || !isfinite(z)) return 0.0;
    /* Base sign(Z)·|Z/σ_z|^kz. kz = 1 is Z/σ_z exactly (no pow call), kz = 0
     * is the sign alone; Z = 0 stays 0 for every kz. */
    double zp = z / sz;
    double kz = zstar_w01(g_status.k_z);
    if (kz < 1.0 && zp != 0.0)
        zp = (zp > 0.0 ? 1.0 : -1.0) * pow(fabs(zp), kz);
    double f  = 1.0;
    double a  = zstar_w01(g_status.k_dn);
    if (isfinite(nsd) && nsd > 0.0 && a > 0.0)
        f *= 1.0 + a * (1.0 / nsd - 1.0);
    double b = zstar_w01(g_status.k_mau);
    if (isfinite(mau) && b > 0.0)
        f *= 1.0 + b * fabs(mau);
    double e = zstar_w01(g_status.k_ser);
    if (isfinite(ser) && e > 0.0)
        f *= 1.0 + e * fabs(ser);
    double d = zstar_w01(g_status.k_disp);
    if (isfinite(disp) && d > 0.0)
        f *= 1.0 + d * fabs(disp);
    double h = zstar_w01(g_status.k_byt);
    if (isfinite(byt) && h > 0.0)
        f *= 1.0 + h * fabs(byt);
    return zp * f;
}

double rank_key(const RunResult *r)
{
    if (!r) return 0.0;
    return zstar_of((double)r->z_ctr, block_sig_of(r), (double)r->node_sd,
                    (double)r->mau, (double)run_ser(r), (double)r->disp,
                    (double)run_byt(r));
}

/* ── Ranking of the held rows by the pool criterion ──────────────────────
 * The extreme set — compaction survivors, /extremes, Save — is chosen on the
 * SAME column and direction that picked the pool: score_sum (Z*, Z, Disp, Δn,
 * Ser, Mau) and score_dir (high = largest values, low = smallest). Both can only
 * change while a scoring runs, so a round's compaction uses exactly the
 * criterion its pool was picked with. ⚠ One tail. On Z* (SUM_KEY) the
 * comparison is |Z*|: high keeps the largest magnitudes, low the smallest.
 * The stored key stays signed, and that is what the cell prints. Items
 * −3, +2.5, −0.4 rank −3, +2.5, −0.4 under high. Every other column stays
 * signed, so under high on Z the most negative values leave.
 * The value is the item's own column as the table shows it; a row without one
 * (NaN: Δn with k < 2, Mau or Ser uncentred) is not
 * ranked and never kept.
 * rank_key() is soft-float double work, and with PASS_KEEP_EXTREME in the
 * thousands a per-slot scan (K × n calls) costs seconds, so each value is
 * computed ONCE and sorted: n calls plus O(n log n).
 * Fills ord[0..return) with row indices, best first. The caller owns ord
 * (NUM_RUNS entries) and any locking. */
typedef struct { double a; uint16_t j; } KeyIdx;

static int keyidx_desc(const void *x, const void *y)
{
    double a = ((const KeyIdx *)x)->a, b = ((const KeyIdx *)y)->a;
    return (a < b) - (a > b);
}

static double item_crit(const RunResult *r, int ch)
{
    switch (ch) {
    case SUM_Z:    return (double)r->z_ctr;
    case SUM_DISP: return (double)r->disp;
    case SUM_NSD:  return (double)r->node_sd;
    case SUM_SER:  return (double)run_ser(r);
    case SUM_BYT:  return (double)run_byt(r);
    case SUM_MAU:  return (double)r->mau;
    default:       return rank_key(r);
    }
}

/* Z* as a RANKING value: NaN when the row's block has no σ — rank_key()
 * gives 0 there, which would top the low end without having been measured
 * as quiet. The one place that rule lives: rank_order(), the full table's
 * frozen key and its scan all go through it. */
static double ranked_key(const RunResult *r)
{
    return block_sig_of(r) > 0.0 ? rank_key(r) : NAN;
}

/* A column value as the sort key of rank_order() and of the full-table scan,
 * larger = better: Z* by magnitude (the row's key is not rewritten), every
 * other column signed; `low` turns the order round. NaN in, NaN out = the
 * row is not ranked on that column. */
static double sort_val(double v, int ch, bool low)
{
    if (!isfinite(v)) return NAN;
    if (ch == SUM_KEY) v = fabs(v);
    return low ? -v : v;
}

static int rank_order(KeyIdx *ord, int ntot, bool need_centred, int ch, bool low)
{
    int m = 0;
    for (int j = 0; j < ntot; j++) {
        const RunResult *r = &g_status.results[j];
        if (!result_ranked(r)) continue;
        if (need_centred && !result_centred(r)) continue;
        double a = sort_val((ch == SUM_KEY) ? ranked_key(r) : item_crit(r, ch), ch, low);
        if (!isfinite(a)) continue;
        ord[m].a = a;
        ord[m].j = (uint16_t)j;
        m++;
    }
    qsort(ord, (size_t)m, sizeof(KeyIdx), keyidx_desc);
    return m;
}

/* ── The live extreme set: the Top-10 and Save  ───────────────────────
 * The up-to `max` ranked & centred rows that rank best on column `ch` in
 * direction `low` (see rank_order), copied into out[0..return) under the
 * archive lock, best first. ch < 0 = the pool criterion, i.e. the live form of
 * the compaction survivors (Save); the Top-10 passes the column the operator
 * clicked, so its ten are the best of every row held, not of a pre-cut set.
 * *held (optional) receives how many rows carried a value at all;
 * *used_ch / *used_low (optional) the column and direction actually used.
 *
 * ⚠ It can only choose from what compaction kept — the pool criterion's 1000
 * plus the open round. Runs once per results-page poll, not per item, on the
 * HTTP task — hence its own scratch (the sensor task's compaction may be
 * selecting at the same moment). result_ranked / result_centred gate it to the
 * same rows every statistic uses, so an uncentred open-block item with its raw
 * node offsets can never appear. */
int results_extremes(RunResult *out, int max, int ch, bool low, int *held,
                     int *used_ch, bool *used_low)
{
    if (ch < 0 || ch >= SCORE_SUM_N) {
        ch  = score_sum_sel();
        low = g_status.score_dir == SCORE_DIR_LOW;
    }
    if (used_ch)  *used_ch  = ch;
    if (used_low) *used_low = low;
    if (held) *held = 0;
    if (!out || max <= 0) return 0;
    static KeyIdx *ord;
    if (!ord) ord = heap_caps_malloc((size_t)NUM_RUNS * sizeof(KeyIdx), MALLOC_CAP_SPIRAM);
    if (!ord) return 0;
    /* sel[] and every caller's buffer are this long. */
    if (max > PASS_KEEP_EXTREME) max = PASS_KEEP_EXTREME;
    archive_lock();
    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;
    int m, total;

    /* The session is over: choose from the full table as well. One pass over
     * it, keeping the `max` best in `sel` (sorted, seeded with the held rows'
     * best) — no sort of the whole table. id ≥ 0 is a results[] row, < 0 the
     * full-table row ~id. While a session runs the held rows are the set: a
     * scan this long belongs nowhere near the consumer core.
     * Nothing moves once the session is over, so the selection is kept and a
     * repeat of the same request (the page polls every 5 s) copies it out
     * without a scan; the next session start drops it (s_sel_valid). */
    typedef struct { double a; int32_t id; } FullSel;
    static FullSel *sel;
    static int c_ch, c_low, c_max, c_n, c_total;
    bool full = g_status.state != ELOTTO_RUNNING && s_full_n > 0;
    if (full && !sel)
        sel = heap_caps_malloc((size_t)PASS_KEEP_EXTREME * sizeof(FullSel), MALLOC_CAP_SPIRAM);
    if (full && sel) {
        if (!(s_sel_valid && c_ch == ch && c_low == (int)low && c_max == max)) {
            m = rank_order(ord, ntot, true, ch, low);
            total = m;
            if (m > max) m = max;
            int c = 0;
            for (; c < m; c++) { sel[c].a = ord[c].a; sel[c].id = (int32_t)ord[c].j; }
            for (int i = 0; i < s_full_n; i++) {
                if (FULL_ROW(i).skip_rank) continue;   /* quarantined: Save only */
                double a = sort_val((ch == SUM_KEY) ? (double)FULL_KEY(i)
                                                    : item_crit(&FULL_ROW(i), ch), ch, low);
                if (!isfinite(a)) continue;
                total++;
                if (c >= max && !(a > sel[c - 1].a)) continue;
                int lo = 0, hi = c;                 /* first slot worse than a */
                while (lo < hi) {
                    int mid = (lo + hi) / 2;
                    if (sel[mid].a >= a) lo = mid + 1; else hi = mid;
                }
                int end = (c < max) ? c : max - 1;
                memmove(&sel[lo + 1], &sel[lo], (size_t)(end - lo) * sizeof(FullSel));
                sel[lo].a  = a;
                sel[lo].id = ~(int32_t)i;
                if (c < max) c++;
            }
            c_ch = ch; c_low = (int)low; c_max = max; c_n = c; c_total = total;
            s_sel_valid = true;
        }
        m = c_n;
        total = c_total;
        for (int i = 0; i < m; i++)
            out[i] = sel[i].id >= 0 ? g_status.results[sel[i].id] : FULL_ROW(~sel[i].id);
    } else {
        m = rank_order(ord, ntot, true, ch, low);
        total = m;
        if (m > max) m = max;
        for (int i = 0; i < m; i++) out[i] = g_status.results[ord[i].j];
    }
    if (held) *held = total;
    archive_unlock();
    return m;
}

/* ── The full table, row by row (Save) ────────────────────────────────────
 * results_full_span() is the index space: the full table's rows, then the
 * rows still in results[]. results_full_rows() copies the ranked, centred
 * items of slots [from, to) — at most `max` — with their Z* (rank_key(), the
 * cell every table prints) under ONE lock, quarantined rows included (the
 * caller flags them by skip_rank), skips a void row still in results[], and
 * sets *next to the slot to go on from. Meant for a
 * finished session — the caller refuses while one runs. */
int results_full_span(void)
{
    archive_lock();
    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;
    int n = s_full_n + ntot;
    archive_unlock();
    return n;
}

int results_full_rows(int from, int to, RunResult *out, double *key, int max, int *next)
{
    int n = 0, i = from < 0 ? 0 : from;
    archive_lock();
    int nf = s_full_n;
    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;
    if (to > nf + ntot) to = nf + ntot;
    for (; i < to && n < max; i++) {
        const RunResult *r;
        if (i < nf) {
            r = &FULL_ROW(i);
        } else {
            r = &g_status.results[i - nf];
            if (r->k == 0 || !result_centred(r)) continue;
        }
        out[n] = *r;
        key[n] = rank_key(r);
        n++;
    }
    archive_unlock();
    *next = i;
    return n;
}

/* ── The full table (FULL_MAX, sensor.h) ─────────────────────────────────
 * Every ranked row pass_compact() merges, in the order it left results[],
 * and every QUARANTINED one (skip_rank — never selected, written by Save
 * with its flag), with its ranked_key() frozen beside it for the scan (the block is closed,
 * its σ and the session's weights are fixed). The CELL is never read from it:
 * every table and Save print rank_key() of the row. A row is here or in
 * results[], never both. Written by the sensor task under the archive lock,
 * read by results_extremes() once the session is over. */
static void full_push(const RunResult *r)
{
    if (s_full_n >= s_full_cap) return;
    FULL_ROW(s_full_n) = *r;
    FULL_KEY(s_full_n) = (float)ranked_key(r);
    s_full_n++;
}

/* ── The session ends by itself ───────────────────────────────────────────
 * At SESSION_MAX_MS of session time, or when the full table has less than one
 * round's worth of rows left. It takes the abort path — the open block is
 * centred, the measured prefix published — but the state is DONE and `fault`
 * and the Log say why. */
static bool s_limit_end;

static void session_end_limit(const char *why)
{
    if (g_status.abort_requested) return;
    /* Appended: `fault` may already name a dropped node, and that must
     * survive the end of the session. */
    size_t fl = strlen(g_status.fault);
    if (fl < sizeof(g_status.fault))
        snprintf(g_status.fault + fl, sizeof(g_status.fault) - fl,
                 "%s%s — session ended", fl ? " · " : "", why);
    printf("%s — session ended\n", why);
    evlog("%s - session ended", why);
    s_limit_end = true;
    g_status.abort_requested = true;
}

static void session_limit_check(void)
{
    if (elapsed_ms_now() >= SESSION_MAX_MS)
        session_end_limit("72 h session limit reached");
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
 * describe the TABLES (top/low, zmax) must not. */
static double s_drop_sum, s_drop_sumsq;
static int    s_drop_n, s_drop_void, s_drop_excl;

/* Recompute pass mean/σ/χ² and Top-N / Bottom-N from the ranked prefix.
 * O(n·TOP_N) after each valid item. Z* is rank_key() itself, so nothing
 * here rescales the tables — no session moments of the key exist. */
static void recompute_pass_ranks(void)
{
    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;
    block_sig_refresh_open();

    /* Seeded with what compaction dropped, so mean/σ/χ² describe every item the
     * session measured and not just the rows still held.
     *
     * ⚠ CENTRED BLOCKS ONLY — see result_centred(). rank_z() is z_ctr, and
     * z_ctr is the provisional RAW value until the block it belongs to has been
     * centred — so an uncentred item carries its nodes' offsets, and a mean or
     * a σ taken over it is measuring the array's own bias, not the null.
     * Everything downstream inherits the gate — pass σ, Σz²/n, the valid-item
     * count — which is the point: they all have to describe one set.
     * ⚠ "Centred" INCLUDES the open block once it is self-centred, from
     * PASS_OPEN_MIN_N (4) items on — so the null evidence carries items whose
     * per-node means come from a handful of measurements (SE ~ sigma/2 at the
     * bar) and is measurably unsettled early in every block, tightening as the
     * block fills and shifting once at the close. Read pass_sigma at a block
     * boundary, not at its start. pass_mean/σ/χ² run on z_ctr.
     * The compaction seeds are safe: pass_compact() runs at a round boundary,
     * i.e. after close_block(), so everything it merged was centred.
     * ⚠ VOID and EXCLUDED are counted over EVERYTHING. They are archive facts,
     * not statistics, and hiding a void run until its block closes would make
     * results[] and the live counter disagree. */
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
        g_status.result_count = 0;
        g_status.comparisons = 0;
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
     * instrument health on the LSB stream.
     *
     * Everything BELOW — the published tables — runs on rank_key() in units
     * of the item's own block σ. top[] is the largest |rank_key|; the copied
     * row keeps the signed key. No session moments of
     * the key are kept: Z* is already standardised, and over a compacted
     * results[] such a mean/σ would describe the surviving EXTREMES, not
     * the session. */
    /* Built into LOCAL lists and published at the end: /status reads
     * top[] from the HTTP task, and zeroing the counts
     * before refilling them let a poll land on an empty or half-built table.
     * The counts still move last, so a racing reader sees either the old list
     * or the new one, never a partial one. */
    RunResult ltop[TOP_N];
    int       ltn = 0;

    for (int j = 0; j < ntot; j++) {
        const RunResult *r = &g_status.results[j];
        if (!result_ranked(r) || !result_centred(r)) continue;

        double mag = fabs(rank_key(r));
        if (ltn < TOP_N || mag > fabs(rank_key(&ltop[ltn - 1]))) {
            if (ltn < TOP_N) ltn++;
            int p = ltn - 1;
            while (p > 0 && fabs(rank_key(&ltop[p - 1])) < mag) {
                ltop[p] = ltop[p - 1];
                p--;
            }
            ltop[p] = *r;
        }

    }

    for (int i = 0; i < ltn; i++) g_status.top[i] = ltop[i];
    g_status.result_count = ltn;

    g_status.comparisons = nv;
}

/* ── Round-boundary compaction ─────────────────────────────────────────────
 * Merge everything except the K best on the pool criterion into moments, so
 * an unlimited session runs until it is aborted instead of stopping at
 * NUM_RUNS. On Z* that order is |rank_key| (see rank_order).
 *
 * Called at every round boundary, after close_block() so every z_ctr is
 * FINAL. n ≤ PASS_KEEP_EXTREME is a no-op, so a short session keeps every row.
 * A second call before a round starts if the next space would not fit in the
 * remaining buffer. Compaction costs rows that can never be recovered.
 *
 * Survivors: the PASS_KEEP_EXTREME items that rank best on the pool criterion
 * and direction (rank_order). Kept in MEASUREMENT ORDER, and s_node_z moves
 * with them.
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

    /* Keep the K best on the pool criterion and direction (rank_order). The
     * scratch is the sensor task's own — the HTTP
     * task's results_extremes() may select concurrently, outside the lock.
     *
     * Ranked rows only. A QUARANTINED row (k > 0, skip_rank) has no reader
     * once its block is closed — it is out of every statistic, and the
     * block σ and the soft-down origins were taken at the close — so it merges
     * into the counters like a void row. */
    KeyIdx *ord = heap_caps_malloc((size_t)n * sizeof(KeyIdx), MALLOC_CAP_SPIRAM);
    if (!ord) {
        heap_caps_free(keep);
        printf("pass: compaction skipped — no heap for the key order\n");
        return;
    }
    int m = rank_order(ord, n, false, score_sum_sel(),
                       g_status.score_dir == SCORE_DIR_LOW);
    if (m > K) m = K;
    for (int i = 0; i < m; i++) keep[ord[i].j] = 1;
    heap_caps_free(ord);

    /* Merge the losers into the moments, then close the gaps in place. Forward
     * copy with w <= j, so the array is rewritten under itself safely and the
     * surviving rows keep their relative order.
     *
     * This whole phase is the critical section: it rewrites results[] and
     * s_node_z in place and then lowers runs_completed, and an HTTP reader
     * (results_extremes) may be walking either array right
     * now. The selection above only reads, so it needs no lock. */
    archive_lock();
    wsig_refresh(true, WSIG_KEEP_N);   /* before any row moves: the board keeps the values */
    int w = 0, dropped = 0;
    for (int j = 0; j < n; j++) {
        const RunResult *r = &g_status.results[j];
        if (!keep[j]) {
            if (r->k == 0)          s_drop_void++;
            else if (r->skip_rank) {
                /* Out of every statistic, but its z is kept: this is the block
                 * a camera was loud in. Save writes it, flagged. */
                s_drop_excl++;
                full_push(r);
            } else {
                double z = rank_z(r);
                s_drop_sum   += z;
                s_drop_sumsq += z * z;
                s_drop_n++;
                /* 0 means "no value for this item", the same convention
                 * rank_key() uses — it must not be counted as one. */
                full_push(r);
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

    /* One compaction merges at most UNLIM_RUNS_MAX rows (NUM_RUNS − the
     * survivors), so this much room guarantees the next one fits whole. */
    if (s_full_cap > 0 && s_full_cap - s_full_n < UNLIM_RUNS_MAX)
        session_end_limit("full table of measurements is full");

    heap_caps_free(keep);

    printf("pass: compacted at round boundary — %d of %d rows dropped, %d kept "
           "(%d items measured, mean %.6f σ %.6f unchanged)\n",
           dropped, n, w, g_status.items_done, g_status.pass_mean, g_status.pass_sigma);
}

/* Collect this window's camera sigma from every node. The master reads
 * its own camera directly; a slave's arrived on its 'Z' reply and nodes.c left
 * it on the node. NaN throughout means "did not report", which is not zero.
 *
 * ⚠ Called right after measure_window() and before anything can trigger the
 * next run, because cam_wsig_now holds ONE window and the next 'M' overwrites
 * it. */
static void wsig_collect(float out[MAX_NODES])
{
    for (int i = 0; i < MAX_NODES; i++) out[i] = NAN;
    out[0] = s_master_wsig;        /* taken in measure_window(), see there */
    for (int i = 1; i < g_status.node_count && i < MAX_NODES; i++)
        out[i] = g_status.nodes[i].cam_wsig_now;
}

/* Offer one item's per-node camera sigmas to the jump board, keeping the
 * WSIG_KEEP_N largest |jump| of the whole session.
 *
 * A fixed insertion-sorted array rather than a pass over results[]: results[]
 * is compacted at every round boundary, so a table computed from it would show
 * the current round and nothing else. The board is the only structure here
 * that outlives compaction, so it copies what it needs.
 *
 * ⚠ One item can put several nodes on the board. That is deliberate: two
 * nodes jumping on the SAME item is a shared cause, one node jumping alone is
 * that camera. Collapsing them to one row per item would erase the
 * distinction that matters most.
 * ⚠ Only a node that was IN the item's combine enters the board — its jump is
 * the only kind that reached a z. Every node's jump still feeds wsig_sd. */
static void wsig_note(const RunResult *r, const float *wsig, node_mask_t mask,
                      uint8_t spass)
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

        /* No floor: the board keeps the largest of whatever happened, and
         * the x-sigma column says whether they matter. See WSIG_KEEP_N. */
        if (!(mask & (1u << i))) continue;        /* not in the combine */

        if (!g_status.wsig_top) continue;

        /* Binary search on |jump|, biggest first, then one memmove. Under the
         * archive lock: wsig_board_copy() reads the whole board from the
         * HTTP task. /status reads the first WSIG_TOP_N unlocked — a torn row
         * shows mixed values for one poll, nothing else. */
        archive_lock();
        WsigEvent *B = g_status.wsig_top;
        int n = g_status.wsig_n, lo = 0, hi = n;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            float e = B[mid].jump;
            if (e < 0.0f) e = -e;
            if (mag > e) hi = mid; else lo = mid + 1;
        }
        int at = lo;
        if (at >= WSIG_KEEP_N) { archive_unlock(); continue; }  /* smaller than every kept row */
        int tail = (n < WSIG_KEEP_N ? n : WSIG_KEEP_N - 1) - at;
        if (tail > 0) memmove(&B[at + 1], &B[at], (size_t)tail * sizeof(WsigEvent));
        if (n < WSIG_KEEP_N) g_status.wsig_n = n + 1;

        WsigEvent *e = &B[at];
        e->round   = r->round;
        e->index   = r->index;
        e->node    = (uint8_t)i;
        e->spass   = spass;
        memcpy(e->nums, r->nums, sizeof(e->nums));
        memcpy(e->euro, r->euro, sizeof(e->euro));
        e->prev = prev;
        e->now  = now;
        e->jump = jump;
        e->t_ms = esp_timer_get_time() / 1000;
        /* Values follow: a pass item from its results[] row (wsig_refresh),
         * a scoring run when its pass closes (wsig_fill_scoring). */
        e->slot = (spass == 0 && r >= g_status.results && r < g_status.results + NUM_RUNS)
                  ? (uint16_t)(r - g_status.results) : (uint16_t)WSIG_NO_SLOT;
        e->k = 0;
        e->z_raw = e->z_ctr = e->disp = e->nsd = e->ser = e->byt = e->mau = e->key = NAN;
        archive_unlock();
    }
}

/* A board row takes its measurement's values. `key` is that measurement's
 * Z*, NaN while it has none. A void run (k = 0) has no z of any kind. */
static void wsig_values(WsigEvent *e, const RunResult *r, float key)
{
    e->k     = r->k;
    e->z_raw = r->k ? (float)r->z_score : NAN;
    e->z_ctr = r->k ? r->z_ctr : NAN;
    e->disp  = r->disp;
    e->nsd   = r->node_sd;
    e->ser   = run_ser(r);
    e->byt   = run_byt(r);
    e->mau   = r->mau;
    e->key   = key;
}

/* Copy a pass item's values from its results[] row. The caller holds the
 * archive lock. `freeze`: compaction is about to move the rows, so the slot
 * stops meaning anything — keep the values, drop the slot. The block is
 * closed by then, so they are final. */
static void wsig_refresh(bool freeze, int limit)
{
    WsigEvent *B = g_status.wsig_top;
    if (!B) return;
    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;
    int n = g_status.wsig_n < limit ? g_status.wsig_n : limit;
    for (int i = 0; i < n; i++) {
        WsigEvent *e = &B[i];
        if (e->spass != 0 || e->slot == WSIG_NO_SLOT) continue;
        if (e->slot < ntot) {
            const RunResult *r = &g_status.results[e->slot];
            if (r->round == e->round && r->index == e->index) {
                wsig_values(e, r, (r->k && result_centred(r)) ? (float)rank_key(r) : NAN);
            }
        }
        if (freeze) e->slot = WSIG_NO_SLOT;
    }
}

/* A scoring pass closed: its runs on the board take that number's values,
 * which the ScoreItem row now holds for exactly this pass. */
static void wsig_fill_scoring(bool euro_pool, uint8_t spass)
{
    WsigEvent *B = g_status.wsig_top;
    if (!B) return;
    archive_lock();
    for (int i = 0; i < g_status.wsig_n; i++) {
        WsigEvent *e = &B[i];
        if (e->spass != spass || e->round != (uint16_t)g_status.round) continue;
        if ((e->euro[0] != 0) != euro_pool) continue;
        const ScoreItem *row = score_row(euro_pool, e->index);
        if (!row) continue;
        wsig_values(e, &row->r, row->key);
    }
    archive_unlock();
}

/* /status: refresh only the rows it publishes (the first WSIG_TOP_N), so the
 * card's Z* is current without walking the whole board on every poll. */
void wsig_board_refresh_top(void)
{
    if (!g_status.wsig_top) return;
    archive_lock();
    wsig_refresh(false, WSIG_TOP_N);
    archive_unlock();
}

int wsig_board_copy(WsigEvent *out, int max)
{
    if (!out || max <= 0 || !g_status.wsig_top) return 0;
    archive_lock();
    wsig_refresh(false, max);
    int n = g_status.wsig_n;
    if (n > max) n = max;
    memcpy(out, g_status.wsig_top, (size_t)n * sizeof(WsigEvent));
    archive_unlock();
    return n;
}

/* Record WHICH measurements carried a tripping block's spread.
 *
 * Called from record_loop() at the moment the trip fires, which is the only
 * moment the answer exists: the block's rows are still in results[] and their
 * per-node z is still in the archive, and one round later compaction has taken
 * both. `block_idx` is 0-based here and stored 1-based, matching what /loops
 * displays.
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
    t->t_ms  = esp_timer_get_time() / 1000;

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

/* Quarantine every measured item in `block_idx` from pass ranking. results[]
 * keeps the rows (skip_rank=1). Called when that block *triggered* a soft-down. */
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

/* Merge one VALID item into the pass (running sums kept for convenience;
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
     * whole round. s_nacc already includes this item -- pairs_add_run() ran
     * before we were called -- and center_block() re-derives every z_ctr in the
     * block from the s_node_z archive, so repeating it per item corrects rather
     * than compounds.
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
 * Moments are centered PER LOOP before merging into the session totals: pooling
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
static void pairs_reset(void)
{
    memset(s_pair, 0, sizeof(s_pair));
    memset(s_nacc, 0, sizeof(s_nacc));
}

/* Merge one run's per-node values in. `z[]` holds each node's own z and `have[]`
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
         * pairs_commit_block closes it. */
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

static void pairs_commit_block(void)
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
 * The regression has n−2 degrees of freedom, so at three points t has ONE —
 * where the 5 % critical value is 12.7, not 2. A flag that fires on noise is
 * worse than no flag. At the 15-min default a full Eurojackpot pass closes
 * ~38 blocks, so this floor clears in ~1.5 h. */
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
 * pairs_commit_block(), which clears the per-block sums this reads. */
static void record_loop(double loop_mean, int loop_idx)
{
    // Per-node mean and σ for this block, straight from the raw sums.
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
        /* Ranking scale of THIS block. Frozen just before this call, so
         * the row can say what rank_key() divided by. 0 = too few items. */
        if (loop_idx >= 0 && loop_idx < BSIG_MAX && s_bsig) {
        }
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
    // whole offset. This IS the drift reference.
    double raw_off = mean_n[0];
    if (g_status.loops_done == 0) g_status.off_first = raw_off;
    g_status.off_last = raw_off;
    g_status.loops_done++;

    drift_add((double)loop_idx, raw_off);

    /* Soft downweight — sticky, min-k protected, trigger-block quarantine.
     *
     * Trip: σ > NODE_SIGMA_SOFT. σ ONLY — |mean| over NODE_MEAN_REPORT is
     * flagged into the block row and printed, and excludes nothing. See
     * NODE_MEAN_REPORT in sensor.h.
     * Clear: NODE_SOFT_CLEAR_BLOCKS consecutive clean blocks, where "clean" is
     * measured against the PEERS in that same block.
     * Floor: never soft-exclude so many that fewer than NODE_SOFT_MIN_COMBINE
     * ok nodes remain eligible; keep the least-bad among candidates.
     * Quarantine: the block that *triggered* a new soft-down is excluded from
     * pass mean/σ/Top-Bottom (results[] still holds every row). Never reboots. */
    bool   want[MAX_NODES] = {false};
    double score[MAX_NODES] = {0};   /* higher = worse; for triage when floor binds */
    bool   have_stats[MAX_NODES] = {false};
    double mean_i[MAX_NODES] = {0}, sig_i[MAX_NODES] = {0};
    double trip_bar = 0.0;
    bool   any_trip = false;
    node_mask_t mean_mask = 0, trip_mask = 0, soft_mask = 0;

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
            mean_mask |= (node_mask_t)(1u << i);
            printf("node %d: block |mean| %.3f over the %.2f report bar "
                   "(σ %.3f, not an exclusion — centred out of the ranking)\n",
                   i, m_i, NODE_MEAN_REPORT, sig);
        }
    }

    /* Trip against THIS block's own median σ. LSB σ is not ~1, so
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
     * that is down is judged against the arms that are up, in the same window. */
    double peer_sig[MAX_NODES];
    int    npeer = 0;
    for (int j = 0; j < g_status.node_count && j < MAX_NODES; j++) {
        if (!g_status.nodes[j].ok || !have_stats[j]) continue;
        if (want[j] || g_status.nodes[j].soft_down) continue;   /* suspect: no reference */
        peer_sig[npeer++] = sig_i[j];
    }
    double clear_sig = NODE_SOFT_CLEAR_SIG;
    if (npeer > 0) {
        for (int a = 1; a < npeer; a++) {          /* insertion sort, n < MAX_NODES */
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
             * this block — i.e. the node was IN the combine while it misbehaved.
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
             * the block's own rows still exist. */
            trip_record(loop_idx, i, mean_i[i], sig_i[i]);
            calibrate_shorten("soft-down");
            trip_mask |= (node_mask_t)(1u << i);
            evlog("Block %d: %s %s (sigma %.2f, bar %.2f)%s", loop_idx + 1,
                  i == 0 ? "master" : g_status.nodes[i].ip,
                  g_status.nodes[i].soft_down ? "still soft-down" : "SOFT-DOWN",
                  sig_i[i], trip_bar,
                  contaminated ? ", block quarantined" : "");
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
                    evlog("Block %d: %s back in the combine after %d clean blocks",
                          loop_idx + 1, i == 0 ? "master" : g_status.nodes[i].ip,
                          NODE_SOFT_CLEAR_BLOCKS);
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
        if (g_status.nodes[i].soft_down) soft_mask |= (node_mask_t)(1u << i);

    if (any_trip) {
        quarantine_block(loop_idx);
        recompute_pass_ranks();
    }

    /* Stamp the verdict onto the row this block already wrote. The LoopStat is
     * filled at the top of record_loop() because it needs the accumulators
     * before they are merged, but the exclusion decision is only reached here —
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
 * which holds this block's per-node sums until pairs_commit_block() clears them at
 * the close.
 *
 * ⚠ Called at every block close AND live during the open block from
 * publish_valid() — s_nacc is a running sum, so the same arithmetic over a
 * partial block is simply a noisier estimate of the same offsets, and it is
 * re-derived from the s_node_z archive every time rather than applied
 * cumulatively. What it does NOT do is decide whether a block counts as
 * centred: that is s_blocks_centred / s_open_centred, set by the callers, so
 * that a live re-centring cannot promote an open block to final.
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

    /* Per-node σ over this block, for the node-agreement column (RunResult.
     * node_sd). Same accumulator and the same >= 2 rule as the means above —
     * a node the block cannot centre is a node the block cannot scale either,
     * so it drops out of the agreement figure instead of entering it raw.
     * ⚠ Needs n >= 3: at n == 2 the sample σ is |z1-z2|/√2 of the very two
     * runs it would then be used to standardise. */
    double sd[MAX_NODES] = {0};
    bool   sdok[MAX_NODES] = {false};
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        if (s_nacc[i].n < 3) continue;
        double n = (double)s_nacc[i].n;
        double mm = s_nacc[i].s / n;
        double v  = (s_nacc[i].ss - n * mm * mm) / (n - 1.0);
        if (v > 0.0) { sd[i] = sqrt(v); sdok[i] = true; }
    }

    /* ⚠ Centring is load-bearing: the per-node LSB bias runs at 1e-3..7e-3 and
     * lands in the z at full size, so uncentred z would rank NODES rather than
     * items.
     * ⚠ And it costs the most: an effect CONSTANT across a block is removed
     * with that offset. What survives is variation between items inside one
     * block — which is the pre-registered bargain. */
    int ntot = g_status.runs_completed;
    if (ntot > NUM_RUNS) ntot = NUM_RUNS;
    int n = 0;
    for (int j = 0; j < ntot; j++) {
        RunResult *r = &g_status.results[j];
        if ((int)r->block != block_idx || r->k == 0) continue;
        const float *row = s_node_z + (size_t)j * MAX_NODES;
        double sum = 0.0, rsum = 0.0;
        int    kk  = 0, rk = 0;
        double u[MAX_NODES];        // per-node deviation in that node's own σ
        int    ku = 0;
        for (int i = 0; i < MAX_NODES; i++) {
            if (!(r->have_mask & (1u << i))) continue;
            if (isnan(row[i])) continue;
            rsum += (double)row[i];
            rk++;
            /* ⚠ A node with fewer than two values in this block has no mean to
             * subtract and stays OUT of the combine: entered raw, its offset
             * (20..95 σ) would be this item's z and would widen the block σ
             * for every other item. Same rule as node_span_col(). */
            if (!ok[i]) continue;
            double c = (double)row[i] - m[i];
            sum += c;
            kk++;
            if (sdok[i]) u[ku++] = c / sd[i];
        }
        if (ku > 0) {
            /* Each node in ITS OWN σ over this block, then Stouffer: every
             * node carries the same weight and the combine has unit variance
             * whichever nodes answered. Unscaled, a loud node sets the sum.
             * A node without a σ (fewer than three values) stays out. */
            double us = 0.0;
            for (int t = 0; t < ku; t++) us += u[t];
            r->z_ctr = (float)(us / sqrt((double)ku));
            n++;
        } else if (kk > 0) {
            /* No node has a σ yet (a block of two items): centred, unscaled. */
            r->z_ctr = (float)(sum / sqrt((double)kk));
            n++;
        } else if (rk > 0) {
            /* No node of this item can be centred (a block of one item): the
             * raw combine stands. Such a block has no σ, so its Z* is 0. */
            r->z_ctr = (float)(rsum / sqrt((double)rk));
            n++;
        }
        /* How far the nodes sat from each other — equivalently from the
         * combined z, which is √k times their mean. Sample σ, so the null is
         * ≈ 1 for independent unit-scaled nodes and a value well under 1 is
         * the "they agreed" case the column exists to show. */
        if (ku >= 2) {
            double um = 0.0;
            for (int t = 0; t < ku; t++) um += u[t];
            um /= (double)ku;
            double vv = 0.0;
            for (int t = 0; t < ku; t++) vv += (u[t] - um) * (u[t] - um);
            vv /= (double)(ku - 1);
            r->node_sd = (float)(vv > 0.0 ? sqrt(vv) : 0.0);
        } else {
            r->node_sd = NAN;
        }

    }
    /* Mau, Ser and Disp over the same items, centred and scaled on this block alone. */
    if (s_node_mau || s_node_ser || s_node_disp || s_node_byt) {
        static int     bidx[NUM_RUNS];
        static node_mask_t bmask[NUM_RUNS];
        static double  bout[NUM_RUNS];
        int nb = 0;
        for (int j = 0; j < ntot; j++) {
            const RunResult *r = &g_status.results[j];
            if ((int)r->block != block_idx || r->k == 0) continue;
            bidx[nb] = j; bmask[nb] = r->have_mask; nb++;
        }
        if (s_node_mau) {
            node_span_col(s_node_mau, bidx, bmask, nb, bout);
            for (int t = 0; t < nb; t++) g_status.results[bidx[t]].mau = (float)bout[t];
        }
        if (s_node_ser) {
            node_span_col(s_node_ser, bidx, bmask, nb, bout);
            for (int t = 0; t < nb; t++) run_ser_set(&g_status.results[bidx[t]], bout[t]);
        }
        if (s_node_byt) {
            node_span_col(s_node_byt, bidx, bmask, nb, bout);
            for (int t = 0; t < nb; t++) run_byt_set(&g_status.results[bidx[t]], bout[t]);
        }
        if (s_node_disp) {
            node_span_col(s_node_disp, bidx, bmask, nb, bout);
            for (int t = 0; t < nb; t++) g_status.results[bidx[t]].disp = (float)bout[t];
        }
    }
    /* Sync: one value per item, centred and scaled on this block alone. */
    if (s_item_sync) {
        double s = 0.0, ss = 0.0;
        int    ns = 0;
        for (int j = 0; j < ntot; j++) {
            const RunResult *r = &g_status.results[j];
            if ((int)r->block != block_idx || r->k == 0) continue;
            double v = (double)s_item_sync[j];
            if (isfinite(v)) { s += v; ss += v * v; ns++; }
        }
        double mu = ns ? s / ns : 0.0, sdv = 0.0;
        if (ns >= 3) {
            double v = (ss - ns * mu * mu) / (ns - 1);
            if (v > 0.0) sdv = sqrt(v);
        }
        for (int j = 0; j < ntot; j++) {
            RunResult *r = &g_status.results[j];
            if ((int)r->block != block_idx || r->k == 0) continue;
            double v = (double)s_item_sync[j];
            run_sync_set(r, (sdv > 0.0 && isfinite(v)) ? (v - mu) / sdv : NAN);
        }
    }
    if (n > 0)
        printf("block %d: centred %d items (node means %+.3f %+.3f %+.3f %+.3f)\n",
               block_idx, n, m[0], m[1], m[2], m[3]);
}

/* ── Block close (v3) ──────────────────────────────────────────────────
 * A block is one round. Closing one centres results[] for that block
 * (z_ctr / node_sd / mau / ser / disp), freezes the per-block σ, appends the /loops
 * row, feeds the drift regression and merges the pairwise moments. Trigger
 * is the round boundary, not a wall clock. */
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
     * pairs_commit_block() clears, so both must run before it. */
    center_block(block_idx);
    /* Final from here on: the sums this block was centred from are about to be
     * cleared, so its z_ctr will not change again. Freeze the ranking σ now,
     * while every row of the block is still in results[] — compaction at the
     * round boundary would otherwise leave rank_key() dividing by the σ of
     * the survivors. */
    if (block_idx + 1 > s_blocks_centred) s_blocks_centred = block_idx + 1;
    s_open_centred = false;              // the next block starts with nothing in it
    block_sig_compute(block_idx, true);
    record_loop(m, block_idx);           // before the clear of the sums
    recompute_pass_ranks();              // centring moved every item in the block
    pairs_commit_block();
    publish_pair_stats();
    s_blk_sum = s_blk_sumsq = 0.0;
    s_blk_n   = 0;
}

/* ── The session (v3): ROUNDS until Abort ─────────────────────────
 *
 * calibrate → Phase 0 scoring → every combination of the scored pool measured
 * EXACTLY ONCE, in one Fisher–Yates random order → score again. No gate
 * between scoring and the pass  and no end condition but Abort. The ROUND
 * boundary is the only block boundary: the pass parks there, the block
 * closes — it is the unit the drift and pairwise diagnostics run on — and the
 * camera sweep runs before the next round scores.
 *
 * results[] fills in MEASUREMENT order (results[j] = j-th item measured, its
 * combination id in .index) and ACCUMULATES across rounds, so the prefix
 * [0..runs_completed) is always the complete record: publishing and an abort
 * both read it directly. ⚠ pass_compact() runs at every round
 * boundary, after which the prefix is the extremes plus survivors plus
 * s_drop_* moments, not every row measured. */
void elotto_task(void *pvParam)
{
    /* BEFORE the state goes RUNNING, which is the moment /status starts serving
     * the pool: the previous SESSION's numbers must not survive into this one.
     * Everything up to the first scoring pass — discovery, the opening sweep —
     * would otherwise show them, and there is nothing on screen to say they
     * are stale. */
    g_status.pool_n_main     = 0;
    g_status.pool_n_euro     = 0;

    g_status.state           = ELOTTO_RUNNING;
    g_status.runs_completed  = 0;
    g_status.items_done      = 0;
    g_status.compacted       = 0;
    s_drop_sum = s_drop_sumsq = 0.0;
    s_drop_n = s_drop_void = s_drop_excl = 0;
    archive_lock();
    s_full_n  = 0;
    s_sel_valid = false;
    archive_unlock();
    s_limit_end = false;
    // unlimited / runs_cap / scoring_passes are session parameters from /start and are NOT reset
    // here — they tag the session.
    g_status.round           = 0;
    g_status.round_base      = 0;
    g_status.round_item_base = 0;
    g_status.round_start_ms  = 0;
    g_status.round_total     = 0;
    g_status.scoring_done    = 0;
    g_status.scoring_pass    = 0;
    g_status.abort_requested = false;
    g_status.elapsed_ms      = 0;
    g_status.slave_connected = false;
    g_status.result_count    = 0;

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
    /* ⚠ Per SESSION, like every counter around it. It is published in /status,
     * so a single timeout left over from a previous run
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
    if (!s_bsig)
        s_bsig = heap_caps_calloc(BSIG_MAX, sizeof(BlockRankSig), MALLOC_CAP_SPIRAM);
    else
        memset(s_bsig, 0, BSIG_MAX * sizeof(BlockRankSig));
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
    /* Per-node z archive in PSRAM. results[] stays internal; this does not. */
    if (!s_node_z)
        s_node_z = heap_caps_malloc((size_t)NUM_RUNS * MAX_NODES * sizeof(float),
                                    MALLOC_CAP_SPIRAM);
    if (s_node_z) {
        for (size_t i = 0; i < (size_t)NUM_RUNS * MAX_NODES; i++)
            s_node_z[i] = NAN;
    }
    /* Window σ per node (Disp), ~56 KB. Missing costs that column only. */
    if (!s_node_disp)
        s_node_disp = heap_caps_malloc((size_t)NUM_RUNS * MAX_NODES * sizeof(float),
                                       MALLOC_CAP_SPIRAM);
    if (s_node_disp) {
        for (size_t i = 0; i < (size_t)NUM_RUNS * MAX_NODES; i++)
            s_node_disp[i] = NAN;
    }
    /* Window Maurer z per node (Mau), ~10 KB. Missing costs that column only. */
    if (!s_node_mau)
        s_node_mau = heap_caps_malloc((size_t)NUM_RUNS * MAX_NODES * sizeof(float),
                                      MALLOC_CAP_SPIRAM);
    if (s_node_mau) {
        for (size_t i = 0; i < (size_t)NUM_RUNS * MAX_NODES; i++)
            s_node_mau[i] = NAN;
    }
    /* Window serial z per node (Ser), the same size. Missing costs that column only. */
    if (!s_node_ser)
        s_node_ser = heap_caps_malloc((size_t)NUM_RUNS * MAX_NODES * sizeof(float),
                                      MALLOC_CAP_SPIRAM);
    if (s_node_ser) {
        for (size_t i = 0; i < (size_t)NUM_RUNS * MAX_NODES; i++)
            s_node_ser[i] = NAN;
    }
    /* Window byte-frequency z per node (Byt), the same size. Missing costs that column only. */
    if (!s_node_byt)
        s_node_byt = heap_caps_malloc((size_t)NUM_RUNS * MAX_NODES * sizeof(float),
                                      MALLOC_CAP_SPIRAM);
    if (s_node_byt) {
        for (size_t i = 0; i < (size_t)NUM_RUNS * MAX_NODES; i++)
            s_node_byt[i] = NAN;
    }
    /* Window synchrony per item (Sync), ~6 KB. Missing costs that column only. */
    if (!s_item_sync)
        s_item_sync = heap_caps_malloc((size_t)NUM_RUNS * sizeof(float), MALLOC_CAP_SPIRAM);
    if (s_item_sync)
        for (int i = 0; i < NUM_RUNS; i++) s_item_sync[i] = NAN;
    /* The board and the per-node history behind it start empty every session:
     * a jump across a reboot or a parameter change is not an event. */
    memset(g_status.trip_hist, 0, sizeof(g_status.trip_hist));
    g_status.trip_n = 0;
    if (!g_status.wsig_top)
        g_status.wsig_top = heap_caps_calloc(WSIG_KEEP_N, sizeof(WsigEvent),
                                             MALLOC_CAP_SPIRAM);
    g_status.wsig_n = 0;
    for (int i = 0; i < MAX_NODES; i++) s_prev_wsig[i] = NAN;
    s_wsig_jsq = 0.0; s_wsig_jn = 0;
    g_status.wsig_sd = 0.0; g_status.wsig_sd_n = 0;
    /* Scoring table: allocated once, emptied per session and again per round
     * (score_rows_begin). */
    if (!g_status.score_rows)
        g_status.score_rows = heap_caps_calloc(SCORE_ROWS_MAX, sizeof(ScoreItem),
                                               MALLOC_CAP_SPIRAM);
    g_status.score_rows_n = 0;
    /* Previous session's sums must not survive into this one. Abort does not
     * come through here — Save still reads them after Abort. */
    score_snap_reset();
    /* score_sum is NOT reset: it holds the stored pool criterion (NVS,
     * loaded at boot, rewritten by POST /scoresum) for the whole session. */
    g_status.pool_used_n = g_status.pool_used_sum = g_status.pool_used_round = 0;
    g_status.score_sig_z = 0.0;
    g_status.score_span_n = 0;
    focus_reset();
    // Block history in PSRAM, for the life of the app (allocated once), so a
    // finished session's table stays readable.
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
    /* Open a new section in this node's own window log. The slaves do
     * the same off their latch transition, which the discovery below is about
     * to trigger — so all four boundaries land within one command of each
     * other without a wire field to carry them. */
    camera_winlog_new_session();
    camera_winlog_reset_record();
    // Re-discover every session: a node that was powered off last time joins
    // this one, and one that vanished does not sit in the table as a phantom.
    nodes_discover();

    // After abort_requested is cleared, so a camera that is not ready can abort
    // the session immediately instead of having the flag wiped by the reset above.
    camera_source_begin();
    g_status.win_n_score = g_status.win_n_pass = 0;

    bool euro    = (g_status.mode == MODE_EUROJACKPOT);
    int  nm      = euro ? 5 : 6;
    int  mx      = euro ? 50 : 49;
    /* ?fix= numbers in every combination; /start validated the range, this
     * only guards against a stale value from a curl-less path. */
    int  fix     = g_status.pool_fix;
    if (fix != 0 && (fix < (euro ? FIX_MIN_50 : FIX_MIN_49) || fix > nm)) fix = 0;
    if (g_status.runs_cap == 0) fix = 0;
    g_status.pool_fix = fix;

    /* passes==0 is the continuous-scoring flag (only legal with maxruns==0),
     * NOT an out-of-range value to reset. */
    if (g_status.scoring_passes != 0 &&
        (g_status.scoring_passes < SCORE_PASSES_MIN || g_status.scoring_passes > SCORE_PASSES_MAX))
        g_status.scoring_passes = SCORE_PASSES_DEFAULT;

    /* ── The scoring-only extension ────────────────────────────────────
     * maxruns==0: no combination pass. The session just scores every number,
     * round after round, endlessly until Abort — the whole results[]/block/
     * compaction machinery below is bypassed, so none of its counters move.
     *   passes>=1  : each round re-scores from scratch (fresh sums).
     *   passes==0  : ONE continuous scoring, one pass per round, sums never
     *                reset (accumulate). */
    bool scoring_only = (g_status.runs_cap == 0);
    bool accumulate   = scoring_only && (g_status.scoring_passes == 0);

    g_status.scoring_total = (accumulate ? 1 : g_status.scoring_passes)
                             * (mx + (euro ? 12 : 0));

    uint8_t pool_main[POOL_SLOTS] = {0};     // fixed mode may hold every number
    uint8_t pool_euro[POOL_EURO_12] = {0};
    // Pool sizes are variables, not constants: unlimited mode derives both
    // from the per-round run cap, and every count downstream is derived from
    // them.
    int     pool_nm = euro ? POOL_MAIN_50 : POOL_MAIN_49;
    int     pool_ne = euro ? POOL_EURO_12 : 0;
    if (scoring_only) {
        /* No pass → no pool. score_run() with pool_size 0 publishes an empty
         * pool; the Top-10 comes straight from the sorted scoring table. */
        pool_nm = pool_ne = 0;
    } else if (g_status.unlimited) {
        if (g_status.runs_cap < 1 || g_status.runs_cap > UNLIM_RUNS_MAX)
            g_status.runs_cap = UNLIM_RUNS_DEFAULT;
        unlimited_pool_sizes(euro, nm, fix, g_status.runs_cap, &pool_nm, &pool_ne);
    }
    g_status.runs_total = scoring_only ? 0
                        : comb(pool_nm - fix, nm - fix) * (euro ? comb(pool_ne, 2) : 1);

    // Pairwise independence check across all nodes (per-block centered)
    pairs_reset();
    session_clock_start();
    evlog("Session started - %s, scoring %.1f s, run %.1f s, %d runs/round, sweep %s, fixed %d",
          euro ? "Eurojackpot" : "6 of 49", g_status.score_target_ms / 1000.0,
          g_status.run_target_ms / 1000.0,
          g_status.runs_cap,
          g_status.cal_budget_ms > 0 ? "on" : "off", fix);

    /* The full table. Without it the session still runs and the Top-10
     * chooses from the held rows after the session as it does during it —
     * said in the Log, after the start line, so the operator can see it. */
    archive_lock();
    int full_cap = 0;
    for (int c = 0; c < FULL_CHUNKS; c++) {
        if (!s_full[c])
            s_full[c] = heap_caps_malloc((size_t)FULL_CHUNK * sizeof(RunResult),
                                         MALLOC_CAP_SPIRAM);
        if (!s_full_key[c])
            s_full_key[c] = heap_caps_malloc((size_t)FULL_CHUNK * sizeof(float),
                                             MALLOC_CAP_SPIRAM);
        if (!s_full[c] || !s_full_key[c]) {
            heap_caps_free(s_full[c]);
            heap_caps_free(s_full_key[c]);
            s_full[c]     = NULL;
            s_full_key[c] = NULL;
            break;
        }
        full_cap += FULL_CHUNK;
    }
    s_full_cap = full_cap;
    archive_unlock();
    /* A short table ends the session early (pass_compact()), so say how short. */
    if (full_cap == 0)
        evlog("No memory for the full table - Top-10 and Save stay on the held rows");
    else if (full_cap < FULL_MAX)
        evlog("Full table: room for %d of %d measurements only", full_cap, FULL_MAX);

    /* Block index of the pass. Declared HERE, before the first goto done, so
     * the abort path can centre the open block (see done:). */
    int block = 0;

    /* ── Opening insertion: the camera sweep ───────────────────────────
     * Before anything is displayed, so every node has its operating point
     * before the first bit that counts. */
    g_status.cal_did_sweep = calibrate_all("session start");
    if (g_status.abort_requested) { slave_abort(); goto done; }

    /* ── Rounds ────────────────────────────────────────────────────────
     * Every session is rounds until Abort: score, measure that pool once,
     * score again. No pool-confirmation gate. Each round re-scores every
     * number from scratch and keeps only as many of the best as fit `runs_cap`
     * measurement runs. Rounds stop on Abort or when results[] is full.
     *
     * results[] keeps filling across rounds — nothing is cleared between them —
     * so every ranking and the pass mean/σ run on the union of all rounds
     * measured so far. Each round is closed as its own block, so centring
     * never mixes items from either side of a re-scoring. */
    int     round          = 0;
    bool    space_full     = false;

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
         * they should not be the ones measured on a stale sweep. This is now the
         * ONLY sweep trigger; `?cal=0` is the no-calibration control. */
        if (round > 1) {
            /* `cal_did_sweep` now means "a sweep ran somewhere in this round"
             * (its /loops row): the round has several candidate points and
             * most of them skip. */
            g_status.cal_did_sweep = false;
            if (calibrate_all("round boundary")) g_status.cal_did_sweep = true;
            if (g_status.abort_requested) { slave_abort(); goto done; }
        }

        /* ── Phase 0: scoring_passes over every number, keys summed  ── */
        g_status.phase = PHASE_SCORING;
        /* accumulate: one pass per round and the rows carry the single growing
         * scoring, so the bar fills over a pass and the rows are not cleared. */
        g_status.scoring_total  = (accumulate ? 1 : g_status.scoring_passes)
                                  * (mx + (euro ? 12 : 0));
        g_status.scoring_done   = 0;
        g_status.scoring_pass   = 0;
        if (!accumulate || round == 1) score_rows_begin(mx, euro);
        /* A visible break between the sweep and the scoring: the page shows the
         * calibration bar finished and the scoring bar at 0 before the first
         * number appears. Nothing is measured; every window flushes the ring
         * first anyway. The break and the scoring clock restart only when a
         * fresh scoring begins — in accumulate mode round>1 just adds another
         * pass to the scoring already running. */
        if (!accumulate || round == 1) {
            vTaskDelay(pdMS_TO_TICKS(SCORE_START_PAUSE_MS));
            if (g_status.abort_requested) { slave_abort(); goto done; }
            g_status.scoring_start_ms = (uint32_t)elapsed_ms_now();
        } else if (g_status.abort_requested) { slave_abort(); goto done; }
        g_status.pool_need_main = (uint8_t)nm;
        g_status.pool_need_euro = euro ? 2 : 0;
        /* Unlimited: the pool sizes come from the run cap, and are re-derived every
         * round because the cap is fixed while nothing else here is. No pass in
         * scoring-only mode, so the pool stays empty. */
        if (g_status.unlimited && !scoring_only)
            unlimited_pool_sizes(euro, nm, fix, g_status.runs_cap, &pool_nm, &pool_ne);
        /* The pool is already withdrawn — at the top of the round, and at
         * session start before the state went RUNNING. Nothing to clear here. */
        memset(pool_main, 0, sizeof(pool_main));
        memset(pool_euro, 0, sizeof(pool_euro));
        /* Keep the accumulator live across rounds in accumulate mode. */
        if (!accumulate || round == 1) {
            s_sacc[0].active = s_sacc[1].active = false;
            s_live_sum = -1;
        }
        int  eff_passes  = accumulate ? 1 : g_status.scoring_passes;
        bool score_reset = !accumulate || (round == 1);
        int  pass_base   = accumulate ? (round - 1) : 0;
        if (g_status.fake_pool && !scoring_only) {
            /* TEST ?fakepool=1: no scoring, a random pool — straight to the pass. */
            fake_pick_pool(mx, pool_nm, pool_main, g_status.pool_main_z);
            if (euro) fake_pick_pool(12, pool_ne, pool_euro, g_status.pool_euro_z);
            evlog("Pool FAKE (random, test)");
        } else {
            int cm = score_run(mx, pool_nm, false, eff_passes, score_reset, pass_base);
            score_snap_put(&s_sacc[0], false, accumulate ? round : cm, accumulate);
            if (g_status.abort_requested) goto done;
            if (euro) {
                int ce = score_run(12, pool_ne, true, eff_passes, score_reset, pass_base);
                score_snap_put(&s_sacc[1], true, accumulate ? round : ce, accumulate);
                if (g_status.abort_requested) goto done;
            }
            /* The pick, on the column selected at the END of the scoring
             * — main and bonus pool alike. No pool without a pass. */
            if (!scoring_only) {
                if (!score_pick_pool(0, pool_main, g_status.pool_main_z, fix)) goto done;
                if (euro && !score_pick_pool(1, pool_euro, g_status.pool_euro_z, 0)) goto done;
            }
        }
        if (scoring_only) {
            /* No pool, no pass: the scoring span is the whole round. Loop back
             * to score again (passes>=1) or to add another pass to the one
             * growing scoring (passes==0). Endless until Abort. */
            g_status.scoring_pass = 0;
            focus_off();
            g_status.elapsed_ms = elapsed_ms_now();
            if (g_status.abort_requested) goto done;
            continue;
        }
        pool_log(pool_main, pool_nm, pool_euro, euro ? pool_ne : 0, fix);
        g_status.pool_used_n     = pool_nm + (euro ? pool_ne : 0);
        g_status.pool_used_sum   = score_sum_sel();
        g_status.pool_used_round = round;
        g_status.scoring_pass = 0;
        focus_off();

        /* Publish the pool that is actually about to be measured — EVERY path.
         * The UI shows it under the scoring bar for the whole run, and in
         * unlimited mode it is the one thing that changes from round to round. */
        g_status.pool_need_main = (uint8_t)nm;
        g_status.pool_need_euro = euro ? 2 : 0;
        for (int i = 0; i < pool_nm; i++) g_status.pool_main[i] = pool_main[i];
        for (int i = 0; i < pool_ne; i++) g_status.pool_euro[i] = pool_euro[i];
        g_status.pool_n_main    = (uint8_t)pool_nm;
        g_status.pool_n_euro    = (uint8_t)pool_ne;

        /* Everything derived from the pool sizes — the operator may have removed
         * numbers, and in unlimited mode the sizes are the cap's answer. At the
         * minimum (pool == draw size) this is exactly ONE combination. */
        int main_combos = comb(pool_nm - fix, nm - fix);
        int euro_combos = euro ? comb(pool_ne, 2) : 1;
        int full_combos = main_combos * euro_combos;
        /* s_perm is NUM_RUNS wide and the shuffle below indexes the WHOLE
         * space, so the space must fit it. A pool above that is a hard stop,
         * not a silent clamp: truncating would measure a subset and publish
         * it as complete. */
        if (full_combos > NUM_RUNS) {
            snprintf(g_status.fault, sizeof(g_status.fault),
                     "combination space %d exceeds NUM_RUNS %d — pool grew past "
                     "its scored proposal; session aborted",
                     full_combos, NUM_RUNS);
            printf("pass: %s\n", g_status.fault);
            g_status.abort_requested = true;
            goto done;
        }

        /* ── The pass's own sweep  ──────────────────────────────────
         * The scoring is 1240 of a Eurojackpot round's ~1340 measurement cycles,
         * so without this the pass — the only phase that writes results[] —
         * would start on an operating point certified the better part of an
         * hour earlier, while `raw_sigma` is non-stationary per node on a
         * timescale of minutes. The sweep costs its budget (10 s
         * default) against a round of tens of minutes.
         *
         * It is NOT a block boundary: the scoring span and the pass are already
         * separate blocks, each standardised by its own σ, so a rung that moves
         * here moves between blocks and never underneath a centring.
         *
         * It also makes the /loops camera fields answerable: camera_calibrate()
         * resets the statistics, so `cam_sig`/`cam_rsig`/`cam_px` on the pass
         * block's row now describe the pass instead of the scoring that
         * dominated the accumulation.
         *
         * Placed after the combination-space check so an aborting round does
         * not pay for a sweep, and before round_start_ms is stamped so the
         * sweep stays out of the measuring clock. */
        if (calibrate_all("before the pass")) g_status.cal_did_sweep = true;
        if (g_status.abort_requested) { slave_abort(); goto done; }

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
         * top of the moments they were already merged into.
         * The tell is n, not sigma -- doubling identical values barely moves
         * mean or sigma, so the sanity check does not catch this. */
        g_status.round_base      = g_status.runs_completed;
        g_status.round_item_base = g_status.items_done;
        /* Stamped HERE and not at `round++`: both sweeps and the scoring pass sit
         * between the two, and neither measures a combination.
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
         * When a round is truncated, the ids measured must not be the
         * lexicographically first ones — in 6-of-49 the combinations built from
         * the pool's lowest numbers, and in Eurojackpot, where mi = i % main_combos
         * and ei = i / main_combos, a truncation below main_combos would pin ei
         * to the FIRST euro pair for the whole round. Only the LAST round of an
         * unlimited session truncates.
         *
         * Forward partial Fisher-Yates: after k steps s_perm[0..k-1] holds a
         * uniformly random k-subset in random order, and at k == full_combos it
         * degenerates to an ordinary full shuffle. */
        for (int i = 0; i < full_combos; i++) s_perm[i] = (uint16_t)i;
        for (int i = 0; i < round_total; i++) {
            int j = i + (int)(fast_rng() % (uint32_t)(full_combos - i));
            uint16_t t = s_perm[i]; s_perm[i] = s_perm[j]; s_perm[j] = t;
        }

        g_status.phase = PHASE_MEASURING;
        for (int j = 0; j < round_total; j++) {
            session_limit_check();
            if (g_status.abort_requested) { slave_abort(); goto done; }
            // Between runs, never inside one: the run just finished is kept and
            // j does not advance, so nothing is lost or duplicated.
            pause_gate();
            if (g_status.abort_requested) { slave_abort(); goto done; }

            /* ⛔ NO mid-round block boundary. A round is one block, so
             * every block of a session holds the same item count and Z/σ
             * compares. The Z* factors are not bounded by (n-1)/√n. */

            /* Uncapped and unrepeated within the round, so slot i IS this
             * round's combination i. */
            int i  = s_perm[j];
            int mi = i % main_combos;
            int ei = euro ? (i / main_combos) : 0;
            int slot = g_status.round_base + j;       // measurement order, session-wide
            RunResult *r = &g_status.results[slot];
            /* ⚠ The slot is reused (compaction, earlier sessions) and
             * Eurojackpot writes only 5 of the 6 nums: clear the tail, or a
             * stale nums[5] shows up as a sixth number. */
            memset(r->nums, 0, sizeof(r->nums));
            if (fix > 0) {
                /* The fixed numbers plus the mi-th choice of the free part,
                 * merged ascending. */
                uint8_t fr[6] = {0};
                if (nm > fix)
                    nth_combination(pool_main + fix, pool_nm - fix, nm - fix, mi, fr);
                for (int a = 0; a < fix; a++) r->nums[a] = pool_main[a];
                for (int a = 0; a < nm - fix; a++) r->nums[fix + a] = fr[a];
                pool_sort(r->nums, nm);
            } else
                nth_combination(pool_main, pool_nm, nm, mi, r->nums);
            if (euro)
                nth_combination(pool_euro, pool_ne, 2, ei, r->euro);
            else
                r->euro[0] = r->euro[1] = 0;

            // The draw goes on the HTML card BEFORE the trigger, so the
            // numbers on screen match the window being sampled.
            focus_publish(FOCUS_DRAW, r->nums, nm, r->euro, euro ? 2 : 0);

            // One broadcast starts every node, then measure locally — all of
            // them integrate the same window, which is the premise the sqrt(n)
            // combine rests on. Every session flushes the ring first, so the
            // bits credited to this item were captured during it.
            // Identical to the scoring window by construction now: both go
            // through measure_window(), which is the point of it.
            WindowMeas w;
            int k = measure_window(&w, false);
            double  z = w.z;
            node_mask_t mask = w.mask;

            /* Archive every node that produced a z (including soft-down ones),
             * so post-hoc recombine is possible. */
            node_z_store(slot, w.znode, w.have);
            node_col_store(s_node_mau, slot, w.mau, w.havemau);
            node_col_store(s_node_ser, slot, w.ser, w.haveser);
            node_col_store(s_node_byt, slot, w.byt, w.havebyt);
            if (s_item_sync && slot >= 0 && slot < NUM_RUNS)
                s_item_sync[slot] = (k > 0) ? (float)w.sync : NAN;
            /* The instrument's own noise while THIS item was measured.
             * Read before anything can trigger the next window: cam_wsig_now
             * holds one window and the next 'M' overwrites it. */
            float wsig[MAX_NODES];
            wsig_collect(wsig);
            if (s_node_disp && slot >= 0 && slot < NUM_RUNS)
                for (int n = 0; n < MAX_NODES; n++)
                    s_node_disp[(size_t)slot * MAX_NODES + n] = wsig[n];
            /* The same window into this node's own ring, tagged with the
             * combination id so /camlog lines up against results[] — which
             * compaction will have eaten by the next round boundary. */
            camera_winlog_push((uint32_t)(i + 1), gcp_last_maurer());
            pairs_add_run(w.znode, w.have);

            r->index     = i + 1;
            r->block     = (uint16_t)block;
            r->round     = (uint16_t)round;
            r->k         = (uint8_t)k;
            r->have_mask = mask;
            r->skip_rank = 0;   /* quarantine only at block close on soft-down trip */
            /* After index/round/have_mask and after nums/euro above, because
             * the board copies all of them: it has to name the measurement
             * without results[], which compaction will have taken. */
            wsig_note(r, wsig, mask, 0);
            run_ser_set(r, NAN);   /* Ser, filled by center_block() */
            run_byt_set(r, NAN);   /* Byt, likewise */
            r->sync_c = RUN_SYNC_NONE;  /* Sync, likewise */
            r->disp = NAN;  /* Disp, likewise */
            r->mau = NAN;   /* Mau, filled by center_block() */
            if (k > 0) {
                r->z_score = z;
                /* Provisional: the block's node means are not known until it
                 * closes, so the live ranking uses the uncentred value and
                 * center_block() replaces it a few minutes later. */
                r->z_ctr   = (float)z;
                /* No block σ per node yet — center_block() fills it. NaN, not
                 * 0: 0 would read as "the nodes agreed perfectly". */
                r->node_sd = NAN;
                s_blk_sum += z;  s_blk_sumsq += z * z;  s_blk_n++;
            } else {
                /* VOID: incomplete combine. Archived with k=0, never enters
                 * ranking, pass mean/σ, or block σ. */
                r->z_score = 0.0;
                r->z_ctr   = 0.0f;
                r->node_sd = NAN;
            }

            g_status.runs_completed = slot + 1;  // AFTER the row is complete: readers
                                                 // (/status, /extremes) trust the rows
            g_status.items_done++;               // session progress; compaction never lowers it
            if (k > 0) publish_valid(r);      // top/low + pass health
            g_status.elapsed_ms     = elapsed_ms_now();
            run_gap_ms(gap_for(false));
        }
        /* The round's final block. Always closed here, so a
         * round is never centred together with the one after it — the pool changed
         * in between, and scoring sat between the two. */
        close_block(block);
        block++;
        pass_compact();
        recompute_pass_ranks();

        if (g_status.abort_requested) break;
        if (space_full || g_status.runs_completed >= NUM_RUNS) {
            snprintf(g_status.fault, sizeof(g_status.fault),
                     "unlimited: results buffer full (%d items) after round %d "
                     "— session ended",
                     g_status.items_done, round);
            printf("%s\n", g_status.fault);
            break;
        }
    }
    goto finalize;

done:
    /* Aborted. results[0..runs_completed) is already compact and already
     * published item by item — nothing to recompute except merging the
     * partial block's pairwise moments so the matrix stays complete. The
     * partial block is deliberately NOT given a /loops row: every stored row
     * describes a full between-insertions span. */
    /* ⚠ Centre the open block FIRST, while s_nacc still holds its per-node
     * sums — pairs_commit_block() below clears them. Without this, an aborted
     * session ranks the open block on RAW z and every closed block on CENTRED
     * z under one common pass mean/σ and Top/Bottom, with no marker to tell them
     * apart. s_blk_n guards the aborts before the measurement pass (opening
     * sweep, scoring), where `block` may not even be
     * initialised and there is nothing to centre. */
    if (s_blk_n > 0) {
        center_block(block);
        /* ⚠ And mark it final, which record_loop() would otherwise have done.
         * The aborted block deliberately gets no /loops row, so loops_done does
         * not move — if the ranking gate read loops_done, an abort in round 1
         * would rank nothing at all over a complete, correctly centred pass. */
        if (block + 1 > s_blocks_centred) s_blocks_centred = block + 1;
        s_open_centred = false;
        block_sig_compute(block, true);
    }
    pairs_commit_block();
    publish_pair_stats();
    recompute_pass_ranks();   /* ranks from the valid prefix */

finalize:
    /* Tell the nodes the session is over. slave_abort() is a broadcast
     * 'A'. Sending it here covers both exits; on the abort path it is the
     * second 'A' and costs one datagram, because the slaves' g_abort is reset
     * at the head of every 'M'/'K' anyway. */
    slave_abort();
    focus_off();
    g_status.paused     = false;
    g_status.elapsed_ms = elapsed_ms_now();
    bool aborted = g_status.abort_requested && !s_limit_end;
    g_status.state = aborted ? ELOTTO_ABORTED : ELOTTO_DONE;
    evlog("Session %s after %d items", aborted ? "aborted" : "done",
          g_status.items_done);
    vTaskDelete(NULL);
}
