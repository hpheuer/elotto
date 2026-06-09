#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "driver/uart.h"
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

/* ── Slave-UART (UART1, TX=GPIO14, RX=GPIO15) ────────────────────────
 * Verkabelung: Master-GPIO14 → Slave-GPIO15
 *              Slave-GPIO14  → Master-GPIO15
 *              GND           ←→ GND
 * ─────────────────────────────────────────────────────────────────── */
#define SLAVE_UART    UART_NUM_1
#define SLAVE_TX_PIN  14
#define SLAVE_RX_PIN  15
#define SLAVE_BAUD    460800

static bool s_slave_ok = false;

static bool slave_readline(char *buf, int maxlen, int timeout_ms)
{
    int        pos = 0;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < end && pos < maxlen - 1) {
        uint8_t ch;
        if (uart_read_bytes(SLAVE_UART, &ch, 1, pdMS_TO_TICKS(10)) > 0) {
            if (ch == '\n') { buf[pos] = '\0'; return true; }
            if (ch != '\r') buf[pos++] = (char)ch;
        }
    }
    buf[pos] = '\0';
    return false;
}

static void slave_init(void)
{
    static bool installed = false;
    if (!installed) {
        uart_config_t cfg = {
            .baud_rate  = SLAVE_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        esp_err_t e = uart_driver_install(SLAVE_UART, 512, 256, 0, NULL, 0);
        if (e != ESP_OK) { g_status.slave_connected = s_slave_ok = false; return; }
        uart_param_config(SLAVE_UART, &cfg);
        uart_set_pin(SLAVE_UART, SLAVE_TX_PIN, SLAVE_RX_PIN,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        installed = true;
    }
    uart_flush_input(SLAVE_UART);
    uart_write_bytes(SLAVE_UART, "P\n", 2);
    char resp[16];
    bool ok = slave_readline(resp, sizeof(resp), 2000);
    s_slave_ok = ok && resp[0] == 'O';
    g_status.slave_connected = s_slave_ok;
    printf("Slave: %s\n", s_slave_ok ? "verbunden" : "nicht erreichbar");
}

static void slave_baseline(int n)
{
    if (!s_slave_ok) return;
    char cmd[16];
    int  len = snprintf(cmd, sizeof(cmd), "B%d\n", n);
    uart_flush_input(SLAVE_UART);
    uart_write_bytes(SLAVE_UART, cmd, len);
    char resp[16];
    if (!slave_readline(resp, sizeof(resp), n * 600 + 10000))
        s_slave_ok = g_status.slave_connected = false;
}

static double slave_measure(void)
{
    char resp[32];
    if (!slave_readline(resp, sizeof(resp), 10000)) {
        s_slave_ok = g_status.slave_connected = false;
        return 0.0;
    }
    if (resp[0] != 'Z' || resp[1] != ':') {
        s_slave_ok = g_status.slave_connected = false;
        return 0.0;
    }
    return atof(resp + 2);
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
    g_status.slave_connected = false;
    if (g_status.baseline_total <= 0 || g_status.baseline_total > 5000)
        g_status.baseline_total = 100;

    slave_init();

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

    /* ── Phase 1b: Slave-Kalibrierung (parallel-unabhängig) ─────────────── */
    if (s_slave_ok && !g_status.abort_requested)
        slave_baseline(g_status.baseline_total);

    /* ── Phase 0: Individuelle Zahlen-Bewertung ──────────────────────── */
    g_status.phase = PHASE_SCORING;
    score_and_build_pool(mx, pool_nm, pool_main);
    if (g_status.abort_requested) goto done;
    if (euro) score_and_build_pool(12, POOL_EURO_12, pool_euro);
    if (g_status.abort_requested) goto done;

    /* ── Phase 2: Alle Kombinationen messen ──────────────────────────── */
    g_status.phase = PHASE_MEASURING;
    for (int i = 0; i < g_status.runs_total; i++) {
        if (g_status.abort_requested) {
            if (s_slave_ok) uart_write_bytes(SLAVE_UART, "A\n", 2);
            break;
        }

        // Kombination deterministisch zuweisen (lexikografische Enumeration)
        int mi = i % main_combos;
        int ei = euro ? (i / main_combos) : 0;
        nth_combination(pool_main, pool_nm, nm, mi, g_status.results[i].nums);
        if (euro)
            nth_combination(pool_euro, POOL_EURO_12, 2, ei, g_status.results[i].euro);
        else
            g_status.results[i].euro[0] = g_status.results[i].euro[1] = 0;

        // Slave triggern, dann eigene Messung (beide laufen zeitgleich)
        bool use_slave = s_slave_ok;
        if (use_slave) uart_write_bytes(SLAVE_UART, "M\n", 2);
        double z = gcp_zscore_raw() - g_status.baseline_mean;
        if (use_slave) {
            double zs = slave_measure();
            if (s_slave_ok) z = (z + zs) * 0.70710678;   // / sqrt(2)
        }

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
