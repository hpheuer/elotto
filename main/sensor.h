#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NUM_RUNS  5000
#define TOP_N       10

typedef enum { MODE_EUROJACKPOT = 0, MODE_LOTTO_649 = 1 } ElottoMode;
typedef enum { ELOTTO_IDLE, ELOTTO_RUNNING, ELOTTO_DONE, ELOTTO_ABORTED } ElottoState;
typedef enum { PHASE_BASELINE, PHASE_MEASURING } ElottoPhase;

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
    int              freq_z2_count;
    uint8_t          freq_nums[6];
    uint8_t          freq_euro[2];
    volatile bool    abort_requested;
    RunResult        results[NUM_RUNS];
} ElottoStatus;

extern ElottoStatus g_status;

void elotto_task(void *pvParam);
