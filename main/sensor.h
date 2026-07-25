#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NUM_RUNS      8000
#define TOP_N           10
#define POOL_MAIN_49    15   // C(15,6) = 5005 combinations
#define POOL_MAIN_50    12   // C(12,5) =  792 combinations
#define POOL_EURO_12     5   // C(5,2)  =   10 combinations

typedef enum { MODE_EUROJACKPOT = 0, MODE_LOTTO_649 = 1 } ElottoMode;
typedef enum { ELOTTO_IDLE, ELOTTO_RUNNING, ELOTTO_DONE, ELOTTO_ABORTED } ElottoState;
typedef enum { PHASE_SCORING, PHASE_BASELINE, PHASE_MEASURING } ElottoPhase;
// Ranking across loops: PEAK = best single-run Z (noise extreme); CUMULATIVE =
// Stouffer Z = Σz/√k per fixed combination (GCP cumulative-deviation method)
typedef enum { RANK_PEAK = 0, RANK_CUMULATIVE = 1 } ElottoRank;
// Measurement bit source. TRNG = on-chip hardware RNG (whitened, opaque);
// CAMERA = OV5647 dark-frame noise (raw, quantum-origin). See docs/PLAN_4NODE.md.
typedef enum { NOISE_TRNG = 0, NOISE_CAMERA = 1 } NoiseSource;

typedef struct {
    int        index;
    double     z_score;
    double     chi_sq;
    const char *p_value;
    uint8_t    nums[6];
    uint8_t    euro[2];
} RunResult;

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
    float    mean_m;       // master-only mean z (after its own baseline subtraction)
    float    mean_s;       // slave-only mean z (after the slave's own baseline)
    float    sig_m, sig_s; // per-node per-run σ over this loop
    float    cam_mbit;     // master camera sustained rate at loop end
    float    s_cam_mbit;   // slave camera rate (D command), 0 = not answered
    uint32_t t_s;          // elapsed seconds at loop end
    uint32_t cam_stalls;   // master camera stalls, cumulative (gate wants 0)
    uint32_t s_cam_stalls; // slave camera stalls, cumulative
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
    double           pair_r;              // master–slave Pearson r over all session pairs (0 = independent)
    int              pair_n;              // number of (z_master, z_slave) pairs collected
    double           sigma_m, sigma_s;    // per-device per-run σ from the pairs
    int              result_count;       // valid entries in top[] (published)
    RunResult        top[TOP_N];          // cumulative highest-Z across loops so far
    int              low_count;           // valid entries in low[] (published)
    RunResult        low[TOP_N];          // cumulative lowest-Z across loops so far
    int              cover_count;         // valid entries in cover[] (cumulative mode only)
    RunResult        cover[TOP_N];        // high-Z but diversified (max-spread) picks
    int              cover_low_count;     // valid entries in cover_low[] (cumulative only)
    RunResult        cover_low[TOP_N];    // low-Z but diversified (max-spread) picks
    volatile bool    abort_requested;
    bool             slave_connected;
    char             slave_ip[16];        // discovered by UDP broadcast ("" = solo)
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
    int              noise_source;        // NoiseSource requested for this session
    volatile bool    noise_stalled;       // camera requested but unavailable/stalled →
                                          // session ABORTED rather than silently
                                          // substituting the TRNG (see PLAN_4NODE
                                          // "Fallback policy": mixing sources mid-session
                                          // changes the measured physics and leaves no
                                          // record of which runs were affected)
    volatile int     slave_source;        // NoiseSource the slave reported on its last
                                          // measurement (it chooses its own, and can
                                          // fall back independently of the master)
    LoopStat        *loop_hist;           // per-loop health table (LOOP_HIST entries,
                                          // PSRAM — internal RAM is full with results[];
                                          // NULL if the allocation failed, in which case
                                          // only the drift/σ aggregates are available)
    RunResult        results[NUM_RUNS];   // live per-loop measurement scratch
} ElottoStatus;

extern ElottoStatus g_status;

void slave_probe(void);
void elotto_task(void *pvParam);
