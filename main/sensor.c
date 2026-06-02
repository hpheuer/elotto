// ==================================================================
//  sensor.c  –  TRNG-Zugriff, GCP-Analyse, Eurojackpot-Extraktion
// ==================================================================
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor.h"

// Hardware-TRNG des ESP32-P4
#define ESP32P4_RNG_DATA_REG  0x501101A4L

static inline uint32_t trng_read_raw(void)
{
    return *((volatile uint32_t *)ESP32P4_RNG_DATA_REG);
}

// ------------------------------------------------------------------
//  GCP-Analyse: 100.000 TRNG-Werte = 3.200.000 Bits = 16.000 Segmente
//  Jedes Segment: 200 Bits, Z-Score = (Einsen - 100) / sqrt(50)
//  Gesamter Lauf-Z-Score = Summe / sqrt(16000)
// ------------------------------------------------------------------
#define TRNG_PER_RUN   100000
#define SEGMENT_BITS   200
#define NUM_SEGMENTS   ((TRNG_PER_RUN * 32) / SEGMENT_BITS)  // 16000

static double gcp_zscore(void)
{
    double z_sum    = 0.0;
    uint32_t word   = 0;
    int bits_left   = 0;

    for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
        int ones = 0;
        for (int b = 0; b < SEGMENT_BITS; b++) {
            if (bits_left == 0) {
                word      = trng_read_raw();
                bits_left = 32;
            }
            ones += word & 1;
            word >>= 1;
            bits_left--;
        }
        z_sum += (ones - 100.0) / 7.07106781;  // sqrt(50)
    }
    return z_sum / sqrt((double)NUM_SEGMENTS);  // sqrt(16000) ≈ 126.49
}

// ------------------------------------------------------------------
//  Lottozahl-Extraktion: Rejection Sampling ohne Modulo-Bias
//  mask muss 2^n - 1 sein mit 2^n >= max_val
// ------------------------------------------------------------------
static uint8_t draw_unbiased(uint8_t max_val, uint8_t mask)
{
    uint8_t v;
    do {
        v = (uint8_t)((trng_read_raw() & mask) + 1);
    } while (v > max_val);
    return v;
}

static void draw_unique_sorted(uint8_t *out, int count,
                               uint8_t max_val, uint8_t mask)
{
    bool used[51] = {false};
    for (int i = 0; i < count; i++) {
        uint8_t v;
        do { v = draw_unbiased(max_val, mask); } while (used[v]);
        used[v] = true;
        out[i] = v;
    }
    // Insertion Sort aufsteigend
    for (int i = 1; i < count; i++) {
        uint8_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j] > key) { out[j + 1] = out[j]; j--; }
        out[j + 1] = key;
    }
}

// ------------------------------------------------------------------
//  p-Wert Label
// ------------------------------------------------------------------
static const char *p_label(double absZ)
{
    if (absZ > 3.29) return "p < 0.001  (hoch signifikant!)";
    if (absZ > 2.58) return "p < 0.01";
    if (absZ > 1.96) return "p < 0.05";
    if (absZ > 1.28) return "p < 0.10   (Trend)";
    return "nicht signifikant";
}

// ------------------------------------------------------------------
//  qsort Vergleich: absteigend nach Z-Score
// ------------------------------------------------------------------
static int cmp_desc(const void *a, const void *b)
{
    const RunResult *ra = (const RunResult *)a;
    const RunResult *rb = (const RunResult *)b;
    if (rb->z_score > ra->z_score) return  1;
    if (rb->z_score < ra->z_score) return -1;
    return 0;
}

// ------------------------------------------------------------------
//  Hauptfunktion
// ------------------------------------------------------------------
void elotto_run(int num_runs)
{
    RunResult *results = calloc(num_runs, sizeof(RunResult));
    if (!results) {
        printf("FEHLER: Kein Speicher!\n");
        return;
    }

    printf("\n");
    printf("╔══════════════════════════════════════════╗\n");
    printf("║          E-Lotto  GCP Analyse            ║\n");
    printf("║   %2d Laeufe  x  100.000 TRNG-Werte      ║\n", num_runs);
    printf("╚══════════════════════════════════════════╝\n\n");

    for (int i = 0; i < num_runs; i++) {
        double z = gcp_zscore();
        results[i].index   = i + 1;
        results[i].z_score = z;
        results[i].chi_sq  = z * z;
        results[i].p_value = p_label(fabs(z));
        printf("  Lauf %2d/%d   Z = %+.4f\n", i + 1, num_runs, z);
        vTaskDelay(1);  // Idle-Task Zeit geben (WDT)
    }

    // Absteigend nach Z-Score sortieren
    qsort(results, num_runs, sizeof(RunResult), cmp_desc);

    // Eurojackpot-Zahlen für Top 5 aus TRNG ziehen
    for (int i = 0; i < TOP_N && i < num_runs; i++) {
        draw_unique_sorted(results[i].main_nums,  5, 50, 63); // 1-50, 6-Bit-Maske
        draw_unique_sorted(results[i].extra_nums, 2, 12, 15); // 1-12, 4-Bit-Maske
    }

    // Ergebnisse ausgeben
    printf("\n");
    printf("╔══════════════════════════════════════════╗\n");
    printf("║     TOP %d  –  positivste GCP-Laeufe     ║\n", TOP_N);
    printf("╚══════════════════════════════════════════╝\n\n");

    for (int i = 0; i < TOP_N && i < num_runs; i++) {
        RunResult *r = &results[i];
        printf("  ── Rang %d  (Lauf %2d) ──────────────────\n",
               i + 1, r->index);
        printf("  Z-Score  :  %+.4f\n",   r->z_score);
        printf("  Chi\xc2\xb2     :  %.4f\n",    r->chi_sq);
        printf("  p-Wert   :  %s\n",      r->p_value);
        printf("  Zahlen   :  %2d - %2d - %2d - %2d - %2d\n",
               r->main_nums[0], r->main_nums[1], r->main_nums[2],
               r->main_nums[3], r->main_nums[4]);
        printf("  Euro     :  %2d - %2d\n\n",
               r->extra_nums[0], r->extra_nums[1]);
    }

    free(results);
    printf("  Fertig.\n\n");
}
