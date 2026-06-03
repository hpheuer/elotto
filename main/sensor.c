#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "sensor.h"

ElottoStatus g_status = { .state = ELOTTO_IDLE };

// Direkter TRNG-Register-Zugriff (75× schneller als esp_random)
// Baseline-Korrektur kompensiert den systematischen Hardware-Bias
#define RNG_REG  (*((volatile uint32_t *)0x501101A4UL))

static inline uint32_t fast_rng(void) { return RNG_REG; }

#define TRNG_PER_RUN   200000
#define SEGMENT_BITS   200
#define NUM_SEGMENTS   ((TRNG_PER_RUN * 32) / SEGMENT_BITS)   // 32000

static const char *p_label(double absZ)
{
    if (absZ > 3.29) return "p&lt;0.001";
    if (absZ > 2.58) return "p&lt;0.01";
    if (absZ > 1.96) return "p&lt;0.05";
    if (absZ > 1.28) return "p&lt;0.10";
    return "n.s.";
}

static double gcp_zscore_raw(void)
{
    double z_sum = 0.0;
    for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
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
    g_status.baseline_done   = 0;
    g_status.abort_requested = false;
    g_status.elapsed_ms      = 0;
    g_status.baseline_mean   = 0.0;
    if (g_status.runs_total    <= 0 || g_status.runs_total    > NUM_RUNS) g_status.runs_total    = 1000;
    if (g_status.baseline_total <= 0 || g_status.baseline_total > 5000)   g_status.baseline_total = 100;

    int64_t t0 = esp_timer_get_time();

    /* ── Phase 1: Baseline-Kalibrierung ──────────────────────────── */
    g_status.phase = PHASE_BASELINE;
    double bsum = 0.0;
    for (int i = 0; i < g_status.baseline_total; i++) {
        if (g_status.abort_requested) goto done;
        bsum += gcp_zscore_raw();
        g_status.baseline_done = i + 1;
        g_status.elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    }
    g_status.baseline_mean = bsum / g_status.baseline_total;

    /* ── Phase 2: Messung mit Baseline-Korrektur ─────────────────── */
    g_status.phase = PHASE_MEASURING;
    int total = g_status.runs_total;

    for (int i = 0; i < total; i++) {
        if (g_status.abort_requested) break;
        double z = gcp_zscore_raw() - g_status.baseline_mean;
        g_status.results[i].index   = i + 1;
        g_status.results[i].z_score = z;
        g_status.results[i].chi_sq  = z * z;
        g_status.results[i].p_value = p_label(fabs(z));
        g_status.runs_completed     = i + 1;
        g_status.elapsed_ms         = (esp_timer_get_time() - t0) / 1000;
    }

done:
    {
        int done = g_status.runs_completed;
        qsort(g_status.results, done, sizeof(RunResult), cmp_desc);
        extract_numbers(done);

        // Frequenz-Analyse: alle Läufe mit Z > 2
        {
            int fm[51] = {0}, fe[13] = {0}, z2 = 0;
            bool euro = (g_status.mode == MODE_EUROJACKPOT);
            int nm = euro ? 5 : 6, mx = euro ? 50 : 49;
            for (int i = 0; i < done; i++) {
                if (g_status.results[i].z_score <= 2.0) break;
                z2++;
                uint8_t t[6] = {0};
                draw_unique_sorted(t, nm, mx, 63);
                for (int j = 0; j < nm; j++) fm[t[j]]++;
                if (euro) {
                    uint8_t et[2] = {0};
                    draw_unique_sorted(et, 2, 12, 15);
                    fe[et[0]]++; fe[et[1]]++;
                }
            }
            g_status.freq_z2_count = z2;
            if (z2 > 0) {
                bool used[51] = {false};
                for (int k = 0; k < nm; k++) {
                    int b = 0, bf = -1;
                    for (int j = 1; j <= mx; j++)
                        if (!used[j] && fm[j] > bf) { b = j; bf = fm[j]; }
                    g_status.freq_nums[k] = (uint8_t)b;
                    if (b) used[b] = true;
                }
                // Aufsteigend sortieren für Anzeige
                for (int i = 1; i < nm; i++) {
                    uint8_t key = g_status.freq_nums[i]; int j = i - 1;
                    while (j >= 0 && g_status.freq_nums[j] > key) { g_status.freq_nums[j+1] = g_status.freq_nums[j]; j--; }
                    g_status.freq_nums[j+1] = key;
                }
                if (euro) {
                    bool eu[13] = {false};
                    for (int k = 0; k < 2; k++) {
                        int b = 0, bf = -1;
                        for (int j = 1; j <= 12; j++)
                            if (!eu[j] && fe[j] > bf) { b = j; bf = fe[j]; }
                        g_status.freq_euro[k] = (uint8_t)b;
                        if (b) eu[b] = true;
                    }
                    if (g_status.freq_euro[0] > g_status.freq_euro[1]) {
                        uint8_t tmp = g_status.freq_euro[0];
                        g_status.freq_euro[0] = g_status.freq_euro[1];
                        g_status.freq_euro[1] = tmp;
                    }
                }
            }
        }

        g_status.elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        g_status.state = g_status.abort_requested ? ELOTTO_ABORTED : ELOTTO_DONE;
    }
    vTaskDelete(NULL);
}
