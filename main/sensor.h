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

typedef struct {
    int        index;
    double     z_score;
    double     chi_sq;
    const char *p_value;
    uint8_t    nums[6];
    uint8_t    euro[2];
} RunResult;

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
    RunResult        results[NUM_RUNS];   // live per-loop measurement scratch
} ElottoStatus;

extern ElottoStatus g_status;

void slave_probe(void);
void elotto_task(void *pvParam);
