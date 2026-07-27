#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "camera.h"

#define NUM_RUNS      8000
#define TOP_N           10
#define POOL_MAIN_49    15   // C(15,6) = 5005 combinations
#define POOL_MAIN_50    12   // C(12,5) =  792 combinations
#define POOL_EURO_12     5   // C(5,2)  =   10 combinations

typedef enum { MODE_EUROJACKPOT = 0, MODE_LOTTO_649 = 1 } ElottoMode;
typedef enum { ELOTTO_IDLE, ELOTTO_RUNNING, ELOTTO_DONE, ELOTTO_ABORTED } ElottoState;
// PHASE_CALIBRATE is appended, not inserted: it runs FIRST in a loop but the
// other three are wired into the UI and the CSV by value.
typedef enum { PHASE_SCORING, PHASE_BASELINE, PHASE_MEASURING,
               PHASE_CALIBRATE, PHASE_POOL_CONFIRM } ElottoPhase;

/* What the operator did with the proposed pool. Written by the /pool handler,
 * read and cleared by elotto_task — one word, so no lock is needed. */
typedef enum { POOL_WAIT = 0, POOL_ACCEPT, POOL_MORE, POOL_CANCEL } PoolAction;

/* Hand the operator's answer to the waiting session. `n_main`/`n_euro` are the
 * numbers still CHECKED; for POOL_MORE they are the ones to keep and omit from
 * the re-scoring pass. Returns false if no session is waiting. */
bool elotto_pool_reply(PoolAction act,
                       const uint8_t *main_sel, int n_main,
                       const uint8_t *euro_sel, int n_euro);
// Ranking across loops: PEAK = best single-run Z (noise extreme); CUMULATIVE =
// Stouffer Z = Σz/√k per fixed combination (GCP cumulative-deviation method)
typedef enum { RANK_PEAK = 0, RANK_CUMULATIVE = 1 } ElottoRank;

/* ENTROPY IS PHOTONS, AND ONLY PHOTONS (user decision, 2026-07-26).
 *
 * The on-chip TRNG is gone from this firmware — not deselected, removed. Every
 * measured bit comes from OV5647 dark-frame shot noise. The reason is the GCP
 * methodology itself: the claim under test is about a *physical* random source,
 * and a whitened hardware RNG is an opaque digital post-process whose output
 * would be indistinguishable from the real thing in every statistic this
 * project computes. Keeping it available as an A/B option meant the codebase
 * could always, in principle, produce a result nobody could attribute.
 *
 * There is therefore no fallback. A node whose camera stops delivering has
 * stopped being an instrument: it is reported as a fault and REBOOTED, never
 * quietly switched to another source. */

typedef struct {
    int        index;
    double     z_score;
    double     chi_sq;
    const char *p_value;
    uint8_t    nums[6];
    uint8_t    euro[2];
} RunResult;

// Focus display (docs/PLAN_4NODE.md Phase 5): what is on screen right now, for
// exactly the window its bits are collected in. The observer is meant to be
// present while the noise is sampled — the original GCP/PEAR protocol — so the
// one property that must hold is `active` ⟺ a run is sampling.
typedef enum { FOCUS_NONE = 0, FOCUS_NUMBER = 1, FOCUS_DRAW = 2 } FocusKind;

// Written by elotto_task, read by the /focus handler on the HTTP task. Not
// locked: `seq` is bumped AFTER the numbers are stored and the reader re-reads
// it, so a torn read is detected rather than served (see focus_publish()).
typedef struct {
    volatile uint32_t seq;      // monotonic; +1 per window. A gap seen by the UI
                                // means a window was missed entirely — the one
                                // failure that credits an effect to the wrong
                                // combination, so it is counted, not smoothed
    volatile uint8_t  active;   // 1 = numbers on screen AND bits being collected
    uint8_t  kind;              // FocusKind
    uint8_t  n, ne;             // numbers in nums[] / euro[]
    uint8_t  nums[6];
    uint8_t  euro[2];
} FocusState;

// Nodes in the array, master included as index 0 (docs/PLAN_NETWORK.md Phase D).
// 4 nodes → C(4,2) = 6 pairwise correlations, which is what the gate checks.
#define MAX_NODES   4
#define MAX_SLAVES  (MAX_NODES - 1)
#define MAX_PAIRS   (MAX_NODES * (MAX_NODES - 1) / 2)

// Per-node health, published so a node that quietly degraded is visible rather
// than merely averaged in. `ok` is session-scoped participation: a node whose
// camera failed is dropped from the combine and stays dropped for the rest of
// the session — it is rebooted, and rejoins by discovery at the next one.
typedef struct {
    char     ip[16];        // discovered by broadcast; "" for the master
    bool     ok;            // still contributing to the combined z
    uint8_t  cam_fault;     // this node's camera stopped delivering bits. It was
                            // dropped and rebooted; the flag stays set for the
                            // rest of the session so the UI can say WHICH node
                            // failed rather than only that the array shrank
    uint32_t reboots;       // times the master power-cycled this node's firmware
                            // over the session. Repeated reboots mean the camera
                            // is not coming back and the hardware needs a look
    double   sigma;         // per-run σ over the session (ideal 1.0)
    uint32_t lost;          // runs this node failed to answer in time
    float    cam_mbit;      // camera rate at the last per-loop 'D' query
    uint32_t cam_stalls;
    // What this node's camera calibration chose at the start of the current loop
    // (PLAN.md Task 1). Nodes land on DIFFERENT settings and that is correct —
    // the cameras are physically different units — which is exactly why the
    // setting has to be published per node rather than as one session number.
    uint32_t cam_exp;       // 0 = this node has not calibrated (yet, or at all)
    uint16_t cam_gain;
    uint8_t  cam_fold;      // XOR fold state chosen
    uint8_t  cam_cal_ok;    // 1 = a candidate passed every gate; 0 = the node
                            // kept its previous setting because none did
    float    cam_bias;      // bias of the window that chose it
    float    cam_cal_mbit;  // rate of that same window
} NodeStatus;

// Per-loop health record (PLAN_4NODE Phase 3). A 20 h session gives slow drift
// far more room than the three loops of Phase 1/2, and drift is the one bias
// form studentize() does NOT absorb: it removes a constant per-loop offset
// exactly, but a trend *across* loops survives. So each completed loop stores
// the numbers a drift check needs — the raw (pre-studentize) offsets and σ per
// node, plus camera health at that moment — and /loops serves the whole table.
#define LOOP_HIST 128            // loops kept in the table; the drift regression
                                 // runs on running sums and is exact beyond it
typedef struct {
    float    base;         // master baseline_mean of this loop = raw per-run z offset
    float    mean;         // combined per-run z mean over the loop (pre-studentize)
    float    sigma;        // combined per-run σ (== loop_sigma), ideal 1.0
    uint8_t  nodes;        // nodes contributing to this loop (master included)
    // Per node, index 0 = master. A node that did not take part leaves zeros,
    // which is distinguishable from a measured 0 by `nodes` and by sig_n == 0.
    float    mean_n[MAX_NODES];   // per-node mean z (after its own baseline)
    float    sig_n[MAX_NODES];    // per-node per-run σ over this loop, ideal 1.0
    float    cam_mbit[MAX_NODES]; // camera rate at loop end, 0 = not answered
    uint32_t cam_stalls[MAX_NODES];
    uint32_t t_s;          // elapsed seconds at loop end
    // Camera settings this loop was MEASURED AT (PLAN.md Task 1 §1.5.2, and
    // mandatory there rather than optional). Per-loop re-tuning is what tracks
    // thermal drift, and studentize() makes it safe for the statistics — but a
    // per-loop change nobody logged is indistinguishable from drift in the data,
    // so the setting travels with the loop it produced.
    uint32_t cam_exp[MAX_NODES];    // 0 = not calibrated this loop
    uint16_t cam_gain[MAX_NODES];
    uint8_t  cam_fold[MAX_NODES];
    uint8_t  cam_cal_ok[MAX_NODES]; // 0 = kept its previous setting, no gate passed
    float    cam_bias[MAX_NODES];   // bias of the window that chose it
    uint16_t cal_ms;       // wall time the whole per-loop calibration cost
    // The measured run window and inter-run gap OF THIS LOOP. Recorded per loop
    // because the count→duration conversion is not stable (open item 4) and
    // per-loop calibration moves the camera's rate on purpose (§1.5.3), so the
    // series across loops is the only way to see the window drift rather than
    // average it away. Measured in every session, attended or not.
    float    win_ms, gap_ms;
} LoopStat;

typedef struct {
    ElottoState      state;
    ElottoPhase      phase;
    ElottoMode       mode;
    volatile int     runs_completed;
    int              runs_total;
    volatile int     baseline_done;
    int              baseline_total;
    double           baseline_mean;
    int64_t          elapsed_ms;
    volatile int     scoring_done;
    int              scoring_total;
    int              freq_z2_count;
    uint8_t          freq_nums[6];
    uint8_t          freq_euro[2];
    int              loops_total;
    volatile int     loop_current;
    int              runs_limit;          // cap on Phase-2 combos (0 = all), for testing
    ElottoRank       rank_mode;           // peak vs cumulative-Z ranking across loops
    double           best_z;              // most extreme |Z| in the published ranking
    double           p_corrected;         // Bonferroni-corrected two-sided p of best_z
    int              comparisons;         // number of comparisons used for correction
    double           loop_sigma;          // empirical per-run σ of last loop (pre-studentize; 1.0 = ideal)
    int              loops_done;          // loops completed and folded into the drift stats
    int              loop_hist_n;         // entries valid in loop_hist[] (<= LOOP_HIST)
    double           drift_slope;         // z-offset change per loop (linear regression on
                                          // the master's raw per-run offset base+mean_m)
    double           drift_t;             // slope / SE(slope); |t| > 3 = real drift, not noise
    double           off_first, off_last; // master raw per-run z offset, first / latest loop
    double           sigma_lo, sigma_hi;  // min / max per-loop combined σ across the session
    // Independence check across ALL node pairs (6 of them at n=4). Only the
    // worst is published as a scalar: the √n gain fails if ANY pair correlates,
    // so the maximum is the number that decides, not an average that would
    // dilute one bad pair among five good ones.
    double           pair_r_max;          // largest |r| over the pairs (signed value kept)
    int              pair_r_i, pair_r_j;  // which two nodes produced it
    int              pair_n;              // runs behind that worst pair
    int              pair_count;          // pairs actually evaluated
    // The FULL matrix, not only the worst pair. The measurement topology is the
    // Risk 1 control — master on isolated power, slaves on one PoE rail — so
    // which pairs correlate is the whole question: slaves-only implicates the
    // shared rail, everything-with-everything implicates the room. Publishing a
    // maximum answers neither. Upper triangle used; index 0 is the master.
    double           pair_r[MAX_NODES][MAX_NODES];
    int              result_count;       // valid entries in top[] (published)
    RunResult        top[TOP_N];          // cumulative highest-Z across loops so far
    int              low_count;           // valid entries in low[] (published)
    RunResult        low[TOP_N];          // cumulative lowest-Z across loops so far
    int              cover_count;         // valid entries in cover[] (cumulative mode only)
    RunResult        cover[TOP_N];        // high-Z but diversified (max-spread) picks
    int              cover_low_count;     // valid entries in cover_low[] (cumulative only)
    RunResult        cover_low[TOP_N];    // low-Z but diversified (max-spread) picks
    volatile bool    abort_requested;
    // ── Focus display (PLAN_4NODE Phase 5) ─────────────────────────────
    bool             focus_mode;          // this session is ATTENDED: the panel is
                                          // live and the session is tagged as such.
                                          // A focus session is not equivalent to an
                                          // unattended one, so the two must never be
                                          // pooled later — hence a recorded flag
                                          // rather than "whether someone was watching"
    volatile bool    paused;              // hold BETWEEN runs (never inside one):
                                          // attention is the scarce resource here, and
                                          // without a pause the only way to stop
                                          // attending is to abort and lose the loop
    int64_t          paused_ms;           // total time held, excluded from elapsed_ms
                                          // so a session with a 40-min break is not
                                          // later read as continuous
    float            focus_win_ms;        // measured mean lit window (the run)
    float            focus_gap_ms;        // measured mean dark gap between runs —
                                          // the gate asks whether the ~200 ms was
                                          // free (existing overhead) or paid for
    FocusState       focus;
    bool             slave_connected;     // at least one slave answered discovery
    int              node_count;          // nodes discovered, master included (>= 1)
    int              node_ok;             // of those, still contributing
    NodeStatus       nodes[MAX_NODES];    // [0] = master
    // UDP transport health (PLAN_NETWORK Phase C / Risk 3: "UDP loss must be
    // handled explicitly, not assumed away"). Per session.
    uint32_t         net_retries;         // commands resent because no reply came
    uint32_t         net_lost;            // triggers with no reply even after the
                                          // resend — the gate wants 0
    uint32_t         net_stale;           // replies dropped for a mismatched
                                          // sequence number, i.e. answers that
                                          // arrived after we stopped waiting.
                                          // Silently accepting one would pair
                                          // z_slave of run k with z_master of
                                          // run k+1 — correlation dressed as
                                          // physics, so they are counted, not used
    // ── Per-loop camera calibration (PLAN.md Task 1) ───────────────────
    int              cal_budget_ms;       // sweep budget per loop, 0 = do not
                                          // calibrate. A no-calibration session
                                          // is the matched control this change
                                          // has to be compared against, so it is
                                          // a session parameter, not a #define
    int              cal_ms;              // what the last loop's calibration
                                          // actually cost, master + ack wait —
                                          // the §1.6 gate is a measured number
    volatile bool    noise_stalled;       // the array lost too many cameras to carry
                                          // on. There is no substitute source to fall
                                          // back to by design, so at n >= 3 a failed
                                          // node is dropped and rebooted and the rest
                                          // continue over √(n−1); below the floor the
                                          // session ABORTS.
    char             fault[112];          // human-readable reason, "" when healthy.
                                          // A camera failure has to reach the operator
                                          // as words — a node silently missing from
                                          // the combine is the failure mode this whole
                                          // policy exists to prevent
    /* ── Attended pool confirmation ────────────────────────────────────
     * Scoring proposes a pool; with `pool_confirm` set the session STOPS at
     * PHASE_POOL_CONFIRM and publishes the proposal here so the operator can
     * edit it before thousands of runs are spent measuring it. Opt-in per
     * session (`POST /start?confirm=1`) because a device that waits for a
     * human would hang every scripted or overnight run — the web UI sends it,
     * curl does not.
     *
     * Keeping fewer numbers is legitimate and shrinks the combination space
     * exactly: at pool_n_main == pool_need_main (and, for Eurojackpot,
     * pool_n_euro == 2) there is exactly ONE combination, and Phase 2 measures
     * that single draw repeatedly — which is the highest-power way to use the
     * instrument, see PLAN.md §1.14 note 3. */
    uint8_t          pool_main[POOL_MAIN_49];   // proposed/confirmed main numbers
    float            pool_main_z[POOL_MAIN_49]; // their scoring z, for display
    uint8_t          pool_euro[POOL_EURO_12];   // Eurojackpot bonus pool
    float            pool_euro_z[POOL_EURO_12];
    uint8_t          pool_n_main;         // slots filled
    uint8_t          pool_n_euro;
    uint8_t          pool_need_main;      // a draw needs this many (5 or 6)
    uint8_t          pool_need_euro;      // 2 for Eurojackpot, 0 for 6-of-49
    uint8_t          pool_confirm;        // 1 = this session stops and asks
    uint8_t          pool_auto;           // 1 = nobody answered; the proposal was
                                          // taken unchanged after the timeout, and
                                          // that fact belongs in the record
    LoopStat        *loop_hist;           // per-loop health table (LOOP_HIST entries,
                                          // PSRAM — internal RAM is full with results[];
                                          // NULL if the allocation failed, in which case
                                          // only the drift/σ aggregates are available)
    RunResult        results[NUM_RUNS];   // live per-loop measurement scratch
} ElottoStatus;

extern ElottoStatus g_status;

void slave_probe(void);
void elotto_task(void *pvParam);

/* The master's most recent calibration sweep, or NULL if it has never run one.
 * The whole per-candidate table, not just the winner: the Task 1 gate is a
 * bias-vs-exposure CURVE, and a single chosen point cannot show whether bias
 * responded to exposure at all. Served by GET /calibrate. */
const camera_cal_t *elotto_last_calibration(void);
