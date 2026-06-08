#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NUM_RUNS      8000
#define TOP_N           10
#define POOL_MAIN_49    15   // C(15,6) = 5005 Kombinationen
#define POOL_MAIN_50    12   // C(12,5) =  792 Kombinationen
#define POOL_EURO_12     5   // C(5,2)  =   10 Kombinationen

typedef enum { MODE_EUROJACKPOT = 0, MODE_LOTTO_649 = 1 } ElottoMode;
typedef enum { ELOTTO_IDLE, ELOTTO_RUNNING, ELOTTO_DONE, ELOTTO_ABORTED } ElottoState;
typedef enum { PHASE_SCORING, PHASE_BASELINE, PHASE_MEASURING } ElottoPhase;

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
    volatile bool    abort_requested;
    bool             slave_connected;
    RunResult        results[NUM_RUNS];
} ElottoStatus;

extern ElottoStatus g_status;

void elotto_task(void *pvParam);
