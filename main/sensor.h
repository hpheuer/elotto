#pragma once
#include <stdint.h>

#define NUM_RUNS   50
#define TOP_N       5

typedef struct {
    int     index;
    double  z_score;
    double  chi_sq;
    const char *p_value;
    uint8_t main_nums[5];   // 5 aus 50
    uint8_t extra_nums[2];  // 2 aus 12
} RunResult;

void elotto_run(int num_runs);
