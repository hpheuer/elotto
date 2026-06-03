#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sensor.h"

ElottoStatus g_status = { .state = ELOTTO_IDLE };

#define TRNG_PER_RUN   200000
#define SEGMENT_BITS   200
#define NUM_SEGMENTS   ((TRNG_PER_RUN * 32) / SEGMENT_BITS)   // 32000

// Batch-Buffer: esp_fill_random füllt 512 Words auf einmal
#define BATCH_SIZE  512
static uint32_t s_rng_buf[BATCH_SIZE];
static int      s_rng_pos = BATCH_SIZE;  // erzwingt initialen Fill

static inline uint32_t fast_rng(void)
{
    if (s_rng_pos >= BATCH_SIZE) {
        esp_fill_random(s_rng_buf, sizeof(s_rng_buf));
        s_rng_pos = 0;
    }
    return s_rng_buf[s_rng_pos++];
}

static const char *p_label(double absZ)
{
    if (absZ > 3.29) return "p&lt;0.001";
    if (absZ > 2.58) return "p&lt;0.01";
    if (absZ > 1.96) return "p&lt;0.05";
    if (absZ > 1.28) return "p&lt;0.10";
    return "n.s.";
}

static double gcp_zscore(void)
{
    double z_sum = 0.0;
    for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
        // 6 × 32 Bit + 1 × 8 Bit = 200 Bit, popcount in einem Takt
        int ones = __builtin_popcount(fast_rng())
                 + __builtin_popcount(fast_rng())
                 + __builtin_popcount(fast_rng())
                 + __builtin_popcount(fast_rng())
                 + __builtin_popcount(fast_rng())
                 + __builtin_popcount(fast_rng())
                 + __builtin_popcount(fast_rng() & 0xFF);
        z_sum += (ones - 100.0) / 7.07106781;
        if (seg % 4000 == 0) vTaskDelay(1);
    }
    return z_sum / sqrt((double)NUM_SEGMENTS);
}

static uint8_t draw_unbiased(uint8_t max_val, uint8_t mask)
{
    uint8_t v;
    do { v = (uint8_t)((fast_rng() & mask) + 1); } while (v > max_val);
    return v;
}

static void draw_unique_sorted(uint8_t *out, int count, uint8_t max_val, uint8_t mask)
{
    bool used[51] = {false};
    for (int i = 0; i < count; i++) {
        uint8_t v;
        do { v = draw_unbiased(max_val, mask); } while (used[v]);
        used[v] = true; out[i] = v;
    }
    for (int i = 1; i < count; i++) {
        uint8_t key = out[i]; int j = i - 1;
        while (j >= 0 && out[j] > key) { out[j+1] = out[j]; j--; }
        out[j+1] = key;
    }
}

static int cmp_desc(const void *a, const void *b)
{
    const RunResult *ra = (const RunResult *)a;
    const RunResult *rb = (const RunResult *)b;
    if (rb->z_score > ra->z_score) return  1;
    if (rb->z_score < ra->z_score) return -1;
    return 0;
}

static void extract_numbers(int done)
{
    int show = done < TOP_N ? done : TOP_N;
    for (int i = 0; i < show; i++) {
        if (g_status.mode == MODE_EUROJACKPOT) {
            draw_unique_sorted(g_status.results[i].nums, 5, 50, 63);
            draw_unique_sorted(g_status.results[i].euro, 2, 12, 15);
        } else {
            draw_unique_sorted(g_status.results[i].nums, 6, 49, 63);
            g_status.results[i].euro[0] = 0;
            g_status.results[i].euro[1] = 0;
        }
    }
}

void elotto_task(void *pvParam)
{
    g_status.state           = ELOTTO_RUNNING;
    g_status.runs_completed  = 0;
    g_status.abort_requested = false;
    g_status.elapsed_ms      = 0;
    if (g_status.runs_total <= 0 || g_status.runs_total > NUM_RUNS)
        g_status.runs_total = NUM_RUNS;

    s_rng_pos = BATCH_SIZE;  // Buffer-Reset bei neuem Lauf

    int64_t t0 = esp_timer_get_time();
    int total = g_status.runs_total;

    for (int i = 0; i < total; i++) {
        if (g_status.abort_requested) break;
        double z = gcp_zscore();
        g_status.results[i].index   = i + 1;
        g_status.results[i].z_score = z;
        g_status.results[i].chi_sq  = z * z;
        g_status.results[i].p_value = p_label(fabs(z));
        g_status.runs_completed     = i + 1;
        g_status.elapsed_ms         = (esp_timer_get_time() - t0) / 1000;
    }

    int done = g_status.runs_completed;
    qsort(g_status.results, done, sizeof(RunResult), cmp_desc);
    extract_numbers(done);

    g_status.elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    g_status.state = g_status.abort_requested ? ELOTTO_ABORTED : ELOTTO_DONE;
    vTaskDelete(NULL);
}
