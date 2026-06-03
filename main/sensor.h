#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NUM_RUNS  5000
#define TOP_N       10

typedef enum { MODE_EUROJACKPOT = 0, MODE_LOTTO_649 = 1 } ElottoMode;

typedef struct {
    int        index;
    double     z_score;
    double     chi_sq;
    const char *p_value;
    uint8_t    nums[6];   // 5 (Eurojackpot) oder 6 (6aus49)
    uint8_t    euro[2];   // nur Eurojackpot
} RunResult;

typedef enum { ELOTTO_IDLE, ELOTTO_RUNNING, ELOTTO_DONE, ELOTTO_ABORTED } ElottoState;

typedef struct {
    ElottoState      state;
    ElottoMode       mode;
    volatile int     runs_completed;
    int              runs_total;
    int64_t          elapsed_ms;
    volatile bool    abort_requested;
    RunResult        results[NUM_RUNS];
} ElottoStatus;

extern ElottoStatus g_status;

void elotto_task(void *pvParam);
