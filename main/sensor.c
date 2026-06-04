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

// Binomialkoeffizient C(n, r) für kleine Werte (max n=15, r=6)
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

// k-te Kombination (0-basiert, lexikografisch) aus sortierten pool[0..n-1], r Elemente
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

// Bewertet jede Zahl 1..max_val per GCP-Lauf, wählt top pool_size Zahlen (aufsteigend sortiert)
static void score_and_build_pool(int max_val, int pool_size, uint8_t *pool)
{
    double scores[51] = {0};
    for (int k = 1; k <= max_val; k++) {
        if (g_status.abort_requested) return;
        scores[k] = gcp_zscore_raw();
        g_status.scoring_done++;
    }
    bool used[51] = {false};
    for (int i = 0; i < pool_size; i++) {
        int b = 0; double bs = -1e18;
        for (int j = 1; j <= max_val; j++)
            if (!used[j] && scores[j] > bs) { b = j; bs = scores[j]; }
        pool[i] = (uint8_t)b;
        if (b) used[b] = true;
    }
    // Aufsteigend sortieren (für konsistente Kombinations-Enumeration)
    for (int i = 1; i < pool_size; i++) {
        uint8_t key = pool[i]; int j = i - 1;
        while (j >= 0 && pool[j] > key) { pool[j+1] = pool[j]; j--; }
        pool[j+1] = key;
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

void elotto_task(void *pvParam)
{
    g_status.state           = ELOTTO_RUNNING;
    g_status.runs_completed  = 0;
    g_status.baseline_done   = 0;
    g_status.scoring_done    = 0;
    g_status.abort_requested = false;
    g_status.elapsed_ms      = 0;
    g_status.baseline_mean   = 0.0;
    if (g_status.baseline_total <= 0 || g_status.baseline_total > 5000)
        g_status.baseline_total = 100;

    bool euro    = (g_status.mode == MODE_EUROJACKPOT);
    int  nm      = euro ? 5 : 6;
    int  mx      = euro ? 50 : 49;
    int  pool_nm = euro ? POOL_MAIN_50 : POOL_MAIN_49;

    g_status.scoring_total = mx + (euro ? 12 : 0);

    // Deklaration vor goto done (C-Regel: keine Sprünge über Initialisierungen)
    uint8_t pool_main[POOL_MAIN_49] = {0};   // 15 Plätze, reicht für beide Modi
    uint8_t pool_euro[POOL_EURO_12] = {0};
    int     main_combos = 0, euro_combos = 1;

    main_combos = comb(pool_nm, nm);
    euro_combos = euro ? comb(POOL_EURO_12, 2) : 1;
    g_status.runs_total = main_combos * euro_combos;

    int64_t t0 = esp_timer_get_time();

    /* ── Phase 1: Baseline-Kalibrierung ──────────────────────────────── */
    g_status.phase = PHASE_BASELINE;
    {
        double bsum = 0.0;
        for (int i = 0; i < g_status.baseline_total; i++) {
            if (g_status.abort_requested) goto done;
            bsum += gcp_zscore_raw();
            g_status.baseline_done = i + 1;
            g_status.elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        }
        g_status.baseline_mean = bsum / g_status.baseline_total;
    }

    /* ── Phase 0: Individuelle Zahlen-Bewertung ──────────────────────── */
    g_status.phase = PHASE_SCORING;
    score_and_build_pool(mx, pool_nm, pool_main);
    if (g_status.abort_requested) goto done;
    if (euro) score_and_build_pool(12, POOL_EURO_12, pool_euro);
    if (g_status.abort_requested) goto done;

    /* ── Phase 2: Alle Kombinationen messen ──────────────────────────── */
    g_status.phase = PHASE_MEASURING;
    for (int i = 0; i < g_status.runs_total; i++) {
        if (g_status.abort_requested) break;

        // Kombination deterministisch zuweisen (lexikografische Enumeration)
        int mi = i % main_combos;
        int ei = euro ? (i / main_combos) : 0;
        nth_combination(pool_main, pool_nm, nm, mi, g_status.results[i].nums);
        if (euro)
            nth_combination(pool_euro, POOL_EURO_12, 2, ei, g_status.results[i].euro);
        else
            g_status.results[i].euro[0] = g_status.results[i].euro[1] = 0;

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

        // Frequenz-Analyse: alle Z>2-Läufe (Kombinationen bereits zugewiesen)
        {
            int fm[51] = {0}, fe[13] = {0}, z2 = 0;
            for (int i = 0; i < done; i++) {
                if (g_status.results[i].z_score <= 2.0) break;
                z2++;
                for (int j = 0; j < nm; j++) fm[g_status.results[i].nums[j]]++;
                if (euro) { fe[g_status.results[i].euro[0]]++; fe[g_status.results[i].euro[1]]++; }
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
