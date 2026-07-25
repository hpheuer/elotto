#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/uart.h"
#include "sensor.h"
#include "camera.h"

ElottoStatus g_status = { .state = ELOTTO_IDLE };

// Per-combination running Σz across loops (cumulative / Stouffer ranking mode)
static double s_zsum[NUM_RUNS];

// Random measurement order for the current loop (Fisher–Yates, rebuilt per
// loop) — decouples slow TRNG drift from fixed combination indices, so drift
// cannot accumulate coherently on specific combinations across loops
static uint16_t s_perm[NUM_RUNS];

// Direct TRNG register access (75× faster than esp_random)
#define RNG_REG  (*((volatile uint32_t *)0x501101A4UL))

static inline uint32_t fast_rng(void) { return RNG_REG; }

#define TRNG_PER_RUN   200000
#define SEGMENT_BITS   200
#define NUM_SEGMENTS   ((TRNG_PER_RUN * 32) / SEGMENT_BITS)   // 32000

// Segments per run is source-dependent (PLAN_4NODE Phase 1). The TRNG is
// oversampled and effectively free, so it keeps the historical 6.4 Mbit/run.
// Camera bits are honest and rate-limited (~3.4 Mbit/s measured), so a run is
// sized for ~0.5 s instead. Run length only sets granularity — statistical
// power per second is rate-limited either way, and z stays N(0,1) because it
// is normalised by √segments.
#define TRNG_SEGMENTS  NUM_SEGMENTS      // 6.4 Mbit/run
#define CAM_SEGMENTS   8000              // 1.6 Mbit/run ≈ 0.47 s at 3.4 Mbit/s

// Which source the running session is actually drawing measurement bits from.
// Distinct from g_status.noise_source (what was *requested*).
static volatile int s_active_source = NOISE_TRNG;

// Camera stall policy: ABORT the session. Substituting the TRNG would swap the
// measured physics — the whole point of camera entropy is replacing the opaque
// whitened TRNG with raw quantum noise — and a session-level flag cannot say
// *which* runs were affected. A stall at run 3 of 2560 would otherwise leave
// 2557 TRNG-sourced runs inside a session still labelled "camera".
static void noise_camera_stalled(void)
{
    g_status.noise_stalled    = true;
    g_status.abort_requested  = true;
    // The in-flight run is discarded by the abort, so the words that finish it
    // do not enter any published result. Switching the local source stops each
    // remaining word from re-paying the 2 s stall timeout while unwinding.
    s_active_source = NOISE_TRNG;
}

static inline uint32_t noise_word(void)
{
    if (s_active_source == NOISE_CAMERA) {
        uint32_t w;
        if (camera_read_word(&w)) return w;
        noise_camera_stalled();
    }
    return RNG_REG;
}

// Called once per session, before any measurement.
static void noise_source_begin(void)
{
    g_status.noise_stalled = false;
    if (g_status.noise_source == NOISE_CAMERA) {
        if (camera_is_ready()) {
            s_active_source = NOISE_CAMERA;
        } else {
            // Refuse to run a "camera" session on TRNG bits.
            noise_camera_stalled();
        }
    } else {
        s_active_source = NOISE_TRNG;
    }
}

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
    const int nseg = (s_active_source == NOISE_CAMERA) ? CAM_SEGMENTS : TRNG_SEGMENTS;
    double z_sum = 0.0;
    for (int seg = 0; seg < nseg; seg++) {
        int ones = __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word() & 0xFF);
        z_sum += (ones - 100.0) / 7.07106781;
        // TRNG path needs explicit yields (4/run, ~40 ms); the camera path
        // already yields inside camera_read_word() whenever it waits on the
        // producer, which is most of the time.
        if (seg % 8000 == 0) vTaskDelay(1);
    }
    return z_sum / sqrt((double)nseg);
}

// Binomial coefficient C(n, r) for small values (max n=15, r=6)
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

// k-th combination (0-based, lexicographic) from sorted pool[0..n-1], r elements
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

// Scores each number 1..max_val with SCORE_REPS GCP runs (Stouffer per number),
// slave-combined like Phase 2 (÷√2), picks the top pool_size numbers (sorted
// ascending). The pool is locked for the whole cumulative session, so this is
// where selection confidence matters most: per-number SE = 1/√(REPS·2), i.e.
// 0.22 at 10 reps and 0.11 at 40, vs 1.0 for a single master-only run.
// Currently 10 to keep the one-time scoring phase short (~7 min for 6/49)
// while the long-run instrumentation is being iterated on; raise it back to 40
// for a session whose pool choice has to be trusted.
#define SCORE_REPS 10
static double score_one_run(void);   // forward (defined after the slave UART block)

static void score_and_build_pool(int max_val, int pool_size, uint8_t *pool)
{
    double scores[51] = {0};
    for (int k = 1; k <= max_val; k++) {
        double sum = 0.0;
        for (int r = 0; r < SCORE_REPS; r++) {
            if (g_status.abort_requested) return;
            sum += score_one_run();
            g_status.scoring_done++;
        }
        scores[k] = sum;   // ranking by Σz ≡ ranking by Stouffer Σz/√R
    }
    bool used[51] = {false};
    for (int i = 0; i < pool_size; i++) {
        int b = 0; double bs = -1e18;
        for (int j = 1; j <= max_val; j++)
            if (!used[j] && scores[j] > bs) { b = j; bs = scores[j]; }
        pool[i] = (uint8_t)b;
        if (b) used[b] = true;
    }
    // Sort ascending (for consistent combination enumeration)
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

static int cmp_asc(const void *a, const void *b)
{
    return -cmp_desc(a, b);
}

/* ── Slave UART (UART1, TX=GPIO14, RX=GPIO15) ────────────────────────
 * Wiring: Master-GPIO14 → Slave-GPIO15
 *         Slave-GPIO14  → Master-GPIO15
 *         GND           ←→ GND
 * ─────────────────────────────────────────────────────────────────── */
#define SLAVE_UART    UART_NUM_1
#define SLAVE_TX_PIN  14
#define SLAVE_RX_PIN  15
#define SLAVE_BAUD    460800   // must match UART_BAUD in elotto_slave/main/slave.c

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
    printf("Slave: %s\n", s_slave_ok ? "connected" : "not reachable");
}

static void slave_baseline_start(int n)
{
    if (!s_slave_ok) return;
    char cmd[16];
    int  len = snprintf(cmd, sizeof(cmd), "B%d\n", n);
    uart_flush_input(SLAVE_UART);
    uart_write_bytes(SLAVE_UART, cmd, len);
}

static void slave_baseline_wait(void)
{
    if (!s_slave_ok) return;
    char resp[16];
    int timeout_ms = g_status.baseline_total * 800 + 15000;
    if (!slave_readline(resp, sizeof(resp), timeout_ms))
        s_slave_ok = g_status.slave_connected = false;
}

static double slave_measure(void)
{
    char resp[40];
    if (!slave_readline(resp, sizeof(resp), 10000)) {
        s_slave_ok = g_status.slave_connected = false;
        return 0.0;
    }
    if (resp[0] != 'Z' || resp[1] != ':') {
        s_slave_ok = g_status.slave_connected = false;
        return 0.0;
    }
    // "Z:<float>,<C|T>" — the trailing source tag is optional so a pre-camera
    // slave still parses. atof() stops at the comma either way.
    const char *tag = strchr(resp, ',');
    g_status.slave_source = (tag && tag[1] == 'C') ? NOISE_CAMERA : NOISE_TRNG;
    // Same policy as a local stall: if this session asked for camera entropy and
    // the slave says it measured on its TRNG, the combined z would mix sources.
    // Abort rather than quietly average them.
    if (g_status.noise_source == NOISE_CAMERA && g_status.slave_source == NOISE_TRNG)
        noise_camera_stalled();
    return atof(resp + 2);
}

/* Ask the slave for its camera health (protocol 'D'). Only ever called between
 * loops, when the slave is idle — never between an 'M' and its 'Z:' reply.
 * A missing/garbled answer is NOT treated as a disconnect: this is diagnostics,
 * and dropping the slave over it would cost the session half its SNR. */
static bool slave_diag(float *mbit, uint32_t *stalls)
{
    if (!s_slave_ok) return false;
    uart_flush_input(SLAVE_UART);
    uart_write_bytes(SLAVE_UART, "D\n", 2);
    char resp[128];
    if (!slave_readline(resp, sizeof(resp), 1500) || resp[0] != 'D' || resp[1] != ':') {
        uart_flush_input(SLAVE_UART);   // drop a late/partial reply so the next
        return false;                   // slave_measure() does not read it
    }
    // "D:<ready>,<bias>,<sigma>,<mbit_s>,<stalls>,<stuck>,<C|T>"
    int   ready = 0, n;
    float bias = 0, sigma = 0, mb = 0;
    unsigned long st = 0, stuck = 0;
    n = sscanf(resp + 2, "%d,%f,%f,%f,%lu,%lu", &ready, &bias, &sigma, &mb, &st, &stuck);
    if (n < 5) return false;
    *mbit   = mb;
    *stalls = (uint32_t)st;
    return true;
}

void slave_probe(void) { slave_init(); }

/* One scoring run, slave-combined like Phase 2: trigger the slave, measure
 * locally in parallel, combine ÷√2. No baseline subtraction — the offset is
 * common to every number and scoring only ranks them. */
static double score_one_run(void)
{
    bool use_slave = s_slave_ok;
    if (use_slave) uart_write_bytes(SLAVE_UART, "M\n", 2);
    double z = gcp_zscore_raw();
    if (use_slave) {
        double zs = slave_measure();
        if (s_slave_ok) z = (z + zs) * 0.70710678;   // / sqrt(2)
    }
    return z;
}

/* Select the most-frequent numbers from the accumulated Z>2 histograms and
 * publish them (sorted ascending) to g_status.freq_*. */
static void publish_frequency(int *fm, int *fe, int z2, int nm, int mx, bool euro)
{
    g_status.freq_z2_count = z2;
    if (z2 <= 0) return;
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
            uint8_t t = g_status.freq_euro[0];
            g_status.freq_euro[0] = g_status.freq_euro[1];
            g_status.freq_euro[1] = t;
        }
    }
}

/* Bonferroni-corrected significance of the most extreme |Z| in the published
 * ranking — honest about the multiple-comparison search over `comparisons`. */
static void compute_significance(int comparisons)
{
    if (comparisons <= 0) {
        g_status.best_z = 0.0; g_status.p_corrected = 1.0; g_status.comparisons = 0;
        return;
    }
    double zt = (g_status.result_count > 0) ? fabs(g_status.top[0].z_score) : 0.0;
    double zb = (g_status.low_count   > 0) ? fabs(g_status.low[0].z_score) : 0.0;
    double zmax = zt > zb ? zt : zb;
    double p1 = erfc(zmax / 1.41421356237);   // two-sided single-test tail prob
    double pc = (double)comparisons * p1;      // Bonferroni
    if (pc > 1.0) pc = 1.0;
    g_status.best_z      = zmax;
    g_status.p_corrected = pc;
    g_status.comparisons = comparisons;
}

/* Studentize one loop's measurements: center on the loop's own mean and scale
 * by the loop's own empirical σ. This (a) removes the common bias offset with
 * a 5005-sample estimate instead of the noisy 100-run baseline (whose error
 * would otherwise accumulate √k-coherently in cumulative mode), and (b) makes
 * per-run Z exactly N(0,1) under the null even if raw source reads are
 * correlated and true σ ≠ 1. Reports the pre-scaling σ as a quality metric. */
static void studentize(int n, double *out_mean)
{
    if (out_mean) *out_mean = 0.0;
    if (n < 4) { g_status.loop_sigma = 0.0; return; }
    double m = 0.0;
    for (int i = 0; i < n; i++) m += g_status.results[i].z_score;
    m /= n;
    double v = 0.0;
    for (int i = 0; i < n; i++) {
        double d = g_status.results[i].z_score - m;
        v += d * d;
    }
    double s = sqrt(v / (n - 1));
    g_status.loop_sigma = s;
    if (out_mean) *out_mean = m;   // the offset removed here — Phase 3 drift check
    if (s < 1e-9) s = 1.0;
    for (int i = 0; i < n; i++) {
        double z = (g_status.results[i].z_score - m) / s;
        g_status.results[i].z_score = z;
        g_status.results[i].chi_sq  = z * z;
        g_status.results[i].p_value = p_label(fabs(z));
    }
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* After a mid-measurement abort the measured entries sit scattered at
 * results[s_perm[0..done-1]] (random order). Compact them into
 * results[0..done-1]: sorted ascending each source index is >= its
 * destination, so the stable forward copy never clobbers unread data. */
static void compact_partial(int done)
{
    qsort(s_perm, done, sizeof(uint16_t), cmp_u16);
    for (int j = 0; j < done; j++)
        g_status.results[j] = g_status.results[s_perm[j]];
}

/* Master–slave independence diagnostics from the (z_m, z_s) pairs collected
 * during Phase 2 — free bookkeeping that verifies the √2 combine assumption.
 * Pairs are centered PER LOOP before folding into the session moments:
 * pooling raw pairs across loops would let each loop's random baseline offset
 * (SE = 1/√n_baseline per device) masquerade as correlation.
 * Publishes Pearson r (should be ~0) and per-device per-run σ (should be ~1). */
typedef struct {
    // per-loop raw sums (reset each loop)
    double lm, ls, lm2, ls2, lms;
    int    ln;
    // session-wide centered second moments
    double cxx, cyy, cxy;
    int    cn, cloops;
} PairStats;

static void pair_fold_loop(PairStats *p)
{
    if (p->ln >= 2) {
        double n  = (double)p->ln;
        double mm = p->lm / n, ms = p->ls / n;
        p->cxx += p->lm2 - n * mm * mm;
        p->cyy += p->ls2 - n * ms * ms;
        p->cxy += p->lms - n * mm * ms;
        p->cn  += p->ln;
        p->cloops++;
    }
    p->lm = p->ls = p->lm2 = p->ls2 = p->lms = 0.0;
    p->ln = 0;
}

static void publish_pair_stats(const PairStats *p)
{
    g_status.pair_n = p->cn;
    int df = p->cn - p->cloops;   // one mean estimated per loop
    if (df < 1) { g_status.pair_r = 0.0; g_status.sigma_m = g_status.sigma_s = 0.0; return; }
    double vm = p->cxx / df, vs = p->cyy / df;
    g_status.sigma_m = vm > 0 ? sqrt(vm) : 0.0;
    g_status.sigma_s = vs > 0 ? sqrt(vs) : 0.0;
    g_status.pair_r  = (p->cxx > 0 && p->cyy > 0) ? p->cxy / sqrt(p->cxx * p->cyy) : 0.0;
}

/* ── Phase 3: cross-loop drift instrumentation ─────────────────────────
 * studentize() removes each loop's own mean exactly, so a *constant* offset
 * (e.g. the camera's residual LSB bias, ≈ −0.33 z/run in Phase 1) is harmless.
 * A trend across loops is not: it survives centering and, over a 20 h session,
 * has far more room to develop than the three loops of Phase 1/2. So regress
 * the master's raw per-run offset on the loop index and publish slope + t.
 * Running sums, so the test stays exact past LOOP_HIST stored loops. */
static struct { double n, sx, sxx, sy, sxy, syy; } s_drift;

static void drift_add(double x, double y)
{
    s_drift.n++;
    s_drift.sx += x; s_drift.sxx += x * x;
    s_drift.sy += y; s_drift.sxy += x * y; s_drift.syy += y * y;
    if (s_drift.n < 3) return;
    double n   = s_drift.n;
    double sxx = s_drift.sxx - s_drift.sx * s_drift.sx / n;
    double sxy = s_drift.sxy - s_drift.sx * s_drift.sy / n;
    double syy = s_drift.syy - s_drift.sy * s_drift.sy / n;
    if (sxx <= 0.0) return;
    double b     = sxy / sxx;
    double resid = syy - b * sxy;              // Σ of squared residuals
    if (resid < 0.0) resid = 0.0;
    double var_b = resid / (n - 2.0) / sxx;    // SE(slope)²
    g_status.drift_slope = b;
    g_status.drift_t     = (var_b > 0.0) ? b / sqrt(var_b) : 0.0;
}

/* Append one completed loop to the health table. Must run BEFORE
 * pair_fold_loop(), which clears the per-loop pair sums this reads. */
static void record_loop(const PairStats *p, double loop_mean, int loop_idx)
{
    double mean_m = loop_mean, mean_s = 0.0, sig_m = 0.0, sig_s = 0.0;
    if (p->ln >= 2) {   // with a slave, split the combined mean per node
        double n = (double)p->ln;
        mean_m = p->lm / n;
        mean_s = p->ls / n;
        double vm = (p->lm2 - n * mean_m * mean_m) / (n - 1.0);
        double vs = (p->ls2 - n * mean_s * mean_s) / (n - 1.0);
        sig_m = vm > 0.0 ? sqrt(vm) : 0.0;
        sig_s = vs > 0.0 ? sqrt(vs) : 0.0;
    }

    camera_stats_t cs;
    camera_get_stats(&cs);
    float    s_mbit   = 0.0f;
    uint32_t s_stalls = 0;
    slave_diag(&s_mbit, &s_stalls);   // slave is idle between loops

    if (g_status.loop_hist && g_status.loop_hist_n < LOOP_HIST) {
        LoopStat *L = &g_status.loop_hist[g_status.loop_hist_n++];
        L->base         = (float)g_status.baseline_mean;
        L->mean         = (float)loop_mean;
        L->sigma        = (float)g_status.loop_sigma;
        L->mean_m       = (float)mean_m;
        L->mean_s       = (float)mean_s;
        L->sig_m        = (float)sig_m;
        L->sig_s        = (float)sig_s;
        L->cam_mbit     = (float)cs.mbit_per_sec;
        L->s_cam_mbit   = s_mbit;
        L->t_s          = (uint32_t)(g_status.elapsed_ms / 1000);
        L->cam_stalls   = cs.stalls;
        L->s_cam_stalls = s_stalls;
    }

    double s = g_status.loop_sigma;
    if (s > 0.0) {
        if (g_status.loops_done == 0 || s < g_status.sigma_lo) g_status.sigma_lo = s;
        if (g_status.loops_done == 0 || s > g_status.sigma_hi) g_status.sigma_hi = s;
    }
    // The master's RAW per-run offset: what the source actually produced this
    // loop, before studentization removed it (baseline + post-baseline residual)
    double raw_off = g_status.baseline_mean + mean_m;
    if (g_status.loops_done == 0) g_status.off_first = raw_off;
    g_status.off_last = raw_off;
    g_status.loops_done++;

    drift_add((double)loop_idx, raw_off);
}

/* Greedy diversified "coverage" picks: from the COVER_POOL most extreme
 * combinations (highest Z if !lowest, most-negative if lowest), choose up to
 * TOP_N that each share at most nm/2 numbers with every already-chosen one —
 * strong by Z but spread out, so the set collectively covers more of the draw
 * space than the (often near-duplicate) raw top-N / bottom-N. Operates on
 * g_status.results[] in combination-index order (cumulative mode). */
#define COVER_POOL 48
static void publish_coverage(int n, int nm, bool lowest,
                             RunResult *out, int *out_count)
{
    if (n <= 0) { *out_count = 0; return; }

    // Gather the COVER_POOL most extreme combinations by Z (best candidate first)
    int candIdx[COVER_POOL];
    int mc = 0;
    for (int i = 0; i < n; i++) {
        double z = g_status.results[i].z_score;
        double worst = (mc > 0) ? g_status.results[candIdx[mc - 1]].z_score : 0.0;
        bool better = (mc < COVER_POOL) || (lowest ? (z < worst) : (z > worst));
        if (!better) continue;
        if (mc < COVER_POOL) mc++;
        int p = mc - 1;
        while (p > 0 && (lowest ? (z < g_status.results[candIdx[p - 1]].z_score)
                                : (z > g_status.results[candIdx[p - 1]].z_score))) {
            candIdx[p] = candIdx[p - 1]; p--;
        }
        candIdx[p] = i;
    }

    int  maxov = nm / 2;
    bool chosen[COVER_POOL] = {false};
    int  sel = 0;
    // Pass 1: greedy by Z, enforce the pairwise overlap constraint
    for (int j = 0; j < mc && sel < TOP_N; j++) {
        RunResult *cj = &g_status.results[candIdx[j]];
        bool ok = true;
        for (int s = 0; s < sel && ok; s++) {
            int shared = 0;
            for (int a = 0; a < nm; a++)
                for (int b = 0; b < nm; b++)
                    if (out[s].nums[a] == cj->nums[b]) shared++;
            if (shared > maxov) ok = false;
        }
        if (ok) { out[sel++] = *cj; chosen[j] = true; }
    }
    // Pass 2: if the constraint was too tight, fill remaining slots by Z
    for (int j = 0; j < mc && sel < TOP_N; j++) {
        if (!chosen[j]) out[sel++] = g_status.results[candIdx[j]];
    }
    *out_count = sel;
}

/* Cumulative (Stouffer) ranking: each of the n fixed combinations has its
 * running Σz in zsum[] over k measured loops. Rank by Z = Σz/√k, publish the
 * top-N / bottom-N and most-frequent (over cumulative Z>2). */
static void publish_cumulative(double *zsum, int n, int k,
                               int *fm, int *fe, int *z2, int nm, int mx, bool euro)
{
    if (k <= 0 || n <= 0) return;
    double sk = sqrt((double)k);
    for (int i = 0; i < n; i++) {
        double z = zsum[i] / sk;                  // Stouffer Z = Σz / √k
        g_status.results[i].z_score = z;
        g_status.results[i].chi_sq  = z * z;
        g_status.results[i].p_value = p_label(fabs(z));
    }

    // Select top-N (highest) and bottom-N (lowest) by insertion, WITHOUT
    // sorting results[] — it must stay in combination-index order so the next
    // loop's Σz accumulation and nums stay aligned by index.
    RunResult top[TOP_N], low[TOP_N];
    int tn = 0, ln = 0;
    for (int i = 0; i < n; i++) {
        RunResult *r = &g_status.results[i];
        if (tn < TOP_N || r->z_score > top[tn ? tn - 1 : 0].z_score) {
            if (tn < TOP_N) tn++;
            int p = tn - 1;
            while (p > 0 && top[p - 1].z_score < r->z_score) { top[p] = top[p - 1]; p--; }
            top[p] = *r;
        }
        if (ln < TOP_N || r->z_score < low[ln ? ln - 1 : 0].z_score) {
            if (ln < TOP_N) ln++;
            int p = ln - 1;
            while (p > 0 && low[p - 1].z_score > r->z_score) { low[p] = low[p - 1]; p--; }
            low[p] = *r;
        }
    }
    for (int i = 0; i < tn; i++) g_status.top[i] = top[i];
    g_status.result_count = tn;
    for (int i = 0; i < ln; i++) g_status.low[i] = low[i];
    g_status.low_count = ln;

    // Most-frequent over cumulative Z>2 (scan, order-independent)
    for (int j = 0; j <= 50; j++) fm[j] = 0;
    for (int j = 0; j <= 12; j++) fe[j] = 0;
    *z2 = 0;
    for (int i = 0; i < n; i++) {
        if (g_status.results[i].z_score <= 2.0) continue;
        (*z2)++;
        for (int j = 0; j < nm; j++) fm[g_status.results[i].nums[j]]++;
        if (euro) { fe[g_status.results[i].euro[0]]++; fe[g_status.results[i].euro[1]]++; }
    }
    publish_frequency(fm, fe, *z2, nm, mx, euro);

    // Diversified max-spread picks from the top-Z and bottom-Z pools
    publish_coverage(n, nm, false, g_status.cover,     &g_status.cover_count);
    publish_coverage(n, nm, true,  g_status.cover_low, &g_status.cover_low_count);
}

/* Fold one completed (or partial) loop's results into the cumulative top-N
 * carry, accumulate the cross-loop Z>2 frequency histograms, and publish the
 * current cumulative top-N + most-frequent so /status can show them between
 * loops (intermediate results), not only at the very end. */
static void absorb_loop(RunResult *carry, int *carry_n,
                        RunResult *low, int *low_n,
                        int *fm, int *fe, int *z2, int nm, int mx, bool euro)
{
    int done = g_status.runs_completed;
    if (done > 0) {
        qsort(g_status.results, done, sizeof(RunResult), cmp_desc);

        // Frequency accumulation over Z>2 runs (sorted desc → stop at <=2)
        for (int i = 0; i < done; i++) {
            if (g_status.results[i].z_score <= 2.0) break;
            (*z2)++;
            for (int j = 0; j < nm; j++) fm[g_status.results[i].nums[j]]++;
            if (euro) { fe[g_status.results[i].euro[0]]++; fe[g_status.results[i].euro[1]]++; }
        }

        int take = done < TOP_N ? done : TOP_N;
        RunResult tmp[2 * TOP_N];
        int tn;

        // Merge this loop's highest-Z into the top carry, keep global top-N
        tn = 0;
        for (int i = 0; i < *carry_n; i++) tmp[tn++] = carry[i];
        for (int i = 0; i < take; i++) tmp[tn++] = g_status.results[i];   // results[] sorted desc
        qsort(tmp, tn, sizeof(RunResult), cmp_desc);
        *carry_n = tn < TOP_N ? tn : TOP_N;
        for (int i = 0; i < *carry_n; i++) carry[i] = tmp[i];

        // Merge this loop's lowest-Z into the low carry, keep global bottom-N
        tn = 0;
        for (int i = 0; i < *low_n; i++) tmp[tn++] = low[i];
        for (int i = 0; i < take; i++) tmp[tn++] = g_status.results[done - take + i];
        qsort(tmp, tn, sizeof(RunResult), cmp_asc);
        *low_n = tn < TOP_N ? tn : TOP_N;
        for (int i = 0; i < *low_n; i++) low[i] = tmp[i];
    }

    // Publish cumulative top-N + bottom-N (entries first, then count)
    for (int i = 0; i < *carry_n; i++) g_status.top[i] = carry[i];
    g_status.result_count = *carry_n;
    for (int i = 0; i < *low_n; i++) g_status.low[i] = low[i];
    g_status.low_count = *low_n;
    g_status.cover_count = g_status.cover_low_count = 0;   // coverage is cumulative-only

    // Publish most-frequent numbers across all loops' Z>2 runs
    publish_frequency(fm, fe, *z2, nm, mx, euro);
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
    g_status.result_count    = 0;
    g_status.low_count       = 0;
    g_status.cover_count     = 0;
    g_status.cover_low_count = 0;
    g_status.freq_z2_count   = 0;
    g_status.loop_current    = 0;
    g_status.best_z          = 0.0;
    g_status.p_corrected     = 1.0;
    g_status.comparisons     = 0;
    g_status.loop_sigma      = 0.0;
    g_status.loops_done      = 0;
    g_status.loop_hist_n     = 0;
    g_status.drift_slope     = 0.0;
    g_status.drift_t         = 0.0;
    g_status.off_first       = 0.0;
    g_status.off_last        = 0.0;
    g_status.sigma_lo        = 0.0;
    g_status.sigma_hi        = 0.0;
    memset(&s_drift, 0, sizeof(s_drift));
    // Loop history lives in PSRAM: results[] already fills internal RAM. Kept
    // for the lifetime of the app (allocated once, never freed) so the table of
    // a finished session survives for inspection.
    if (!g_status.loop_hist)
        g_status.loop_hist = heap_caps_calloc(LOOP_HIST, sizeof(LoopStat), MALLOC_CAP_SPIRAM);
    g_status.pair_r          = 0.0;
    g_status.pair_n          = 0;
    g_status.sigma_m         = 0.0;
    g_status.sigma_s         = 0.0;
    if (g_status.baseline_total <= 0 || g_status.baseline_total > 5000)
        g_status.baseline_total = 100;
    if (g_status.loops_total <= 0 || g_status.loops_total > 500)
        g_status.loops_total = 1;

    slave_init();

    // After abort_requested is cleared, so a camera that is not ready can abort
    // the session immediately instead of having the flag wiped by the reset above.
    noise_source_begin();

    bool euro    = (g_status.mode == MODE_EUROJACKPOT);
    int  nm      = euro ? 5 : 6;
    int  mx      = euro ? 50 : 49;
    int  pool_nm = euro ? POOL_MAIN_50 : POOL_MAIN_49;

    g_status.scoring_total = (mx + (euro ? 12 : 0)) * SCORE_REPS;

    uint8_t pool_main[POOL_MAIN_49] = {0};   // 15 slots, enough for both modes
    uint8_t pool_euro[POOL_EURO_12] = {0};
    int     main_combos = comb(pool_nm, nm);
    int     euro_combos = euro ? comb(POOL_EURO_12, 2) : 1;
    int     full_combos = main_combos * euro_combos;
    g_status.runs_total = (g_status.runs_limit > 0 && g_status.runs_limit < full_combos)
                          ? g_status.runs_limit : full_combos;

    // Peak-mode carries (best/worst single-run Z across loops) + freq histograms
    RunResult carry[TOP_N];
    RunResult low_carry[TOP_N];
    int carry_n = 0, low_n = 0;
    int fm[51] = {0}, fe[13] = {0}, z2 = 0;

    // Cumulative (Stouffer) mode: fixed pool, Σz per combination over meas_k loops
    bool cumulative = (g_status.rank_mode == RANK_CUMULATIVE);
    int  meas_k = 0;
    if (cumulative)
        memset(s_zsum, 0, sizeof(double) * (size_t)g_status.runs_total);

    // Master–slave pair stats for the independence check (per-loop centered)
    PairStats pairs = {0};

    int comparisons = 0;
    int64_t t0 = esp_timer_get_time();

    for (int loop = 0; loop < g_status.loops_total; loop++) {
        g_status.loop_current   = loop + 1;
        g_status.baseline_done  = 0;
        g_status.scoring_done   = 0;
        g_status.runs_completed = 0;
        g_status.baseline_mean  = 0.0;
        // Cumulative mode locks the pool after loop 0; peak mode rebuilds it
        bool do_score = (!cumulative || loop == 0);
        if (do_score) {
            memset(pool_main, 0, sizeof(pool_main));
            memset(pool_euro, 0, sizeof(pool_euro));
        }

        /* ── Phase 1: Baseline calibration (Master + Slave in parallel) ── */
        g_status.phase = PHASE_BASELINE;
        if (s_slave_ok) slave_baseline_start(g_status.baseline_total);
        {
            double bsum = 0.0;
            for (int i = 0; i < g_status.baseline_total; i++) {
                if (g_status.abort_requested) {
                    if (s_slave_ok) uart_write_bytes(SLAVE_UART, "A\n", 2);
                    goto done;
                }
                bsum += gcp_zscore_raw();
                g_status.baseline_done = i + 1;
                g_status.elapsed_ms = (esp_timer_get_time() - t0) / 1000;
            }
            g_status.baseline_mean = bsum / g_status.baseline_total;
        }
        if (s_slave_ok && !g_status.abort_requested)
            slave_baseline_wait();

        /* ── Phase 0: Individual number scoring (cumulative: loop 0 only) ── */
        g_status.phase = PHASE_SCORING;
        if (do_score) {
            score_and_build_pool(mx, pool_nm, pool_main);
            if (g_status.abort_requested) goto done;
            if (euro) score_and_build_pool(12, POOL_EURO_12, pool_euro);
            if (g_status.abort_requested) goto done;
        } else {
            g_status.scoring_done = g_status.scoring_total;   // pool already locked
        }

        /* ── Phase 2: Measure all combinations in fresh random order ───── */
        // Fisher–Yates: with a fixed order, slow drift (temperature ramp over
        // the ~20-min loop) would hit each combination at the same position
        // every loop and accumulate √k-coherently — exactly like a real signal.
        for (int i = 0; i < g_status.runs_total; i++) s_perm[i] = (uint16_t)i;
        for (int i = g_status.runs_total - 1; i > 0; i--) {
            // Deliberately fast_rng(), not noise_word(): this is administrative
            // randomness (measurement order), not measured data. Spending
            // rate-limited camera entropy here would stall the session for
            // bits that never enter a z-score.
            int j = (int)(fast_rng() % (uint32_t)(i + 1));
            uint16_t t = s_perm[i]; s_perm[i] = s_perm[j]; s_perm[j] = t;
        }

        g_status.phase = PHASE_MEASURING;
        for (int j = 0; j < g_status.runs_total; j++) {
            if (g_status.abort_requested) {
                if (s_slave_ok) uart_write_bytes(SLAVE_UART, "A\n", 2);
                goto done;
            }
            int i = s_perm[j];   // slot index; results[] stays slot-indexed

            // Slot → combination: spread slots evenly over the full space so a
            // Runs cap samples across all combinations instead of taking the
            // lexicographic prefix (which all shares the pool's lowest numbers).
            // Uncapped, runs_total == full_combos and c == i exactly.
            int c  = (int)(((int64_t)i * full_combos) / g_status.runs_total);
            int mi = c % main_combos;
            int ei = euro ? (c / main_combos) : 0;
            nth_combination(pool_main, pool_nm, nm, mi, g_status.results[i].nums);
            if (euro)
                nth_combination(pool_euro, POOL_EURO_12, 2, ei, g_status.results[i].euro);
            else
                g_status.results[i].euro[0] = g_status.results[i].euro[1] = 0;

            // Trigger slave, then measure locally (both run simultaneously)
            bool use_slave = s_slave_ok;
            if (use_slave) uart_write_bytes(SLAVE_UART, "M\n", 2);
            double zm = gcp_zscore_raw() - g_status.baseline_mean;
            double z  = zm;
            if (use_slave) {
                double zs = slave_measure();
                if (s_slave_ok) {
                    pairs.ln++; pairs.lm += zm; pairs.ls += zs;   // independence check
                    pairs.lm2 += zm * zm; pairs.ls2 += zs * zs; pairs.lms += zm * zs;
                    z = (zm + zs) * 0.70710678;                   // / sqrt(2)
                }
            }

            g_status.results[i].index   = i + 1;
            g_status.results[i].z_score = z;
            g_status.results[i].chi_sq  = z * z;
            g_status.results[i].p_value = p_label(fabs(z));
            g_status.runs_completed     = j + 1;
            g_status.elapsed_ms         = (esp_timer_get_time() - t0) / 1000;
        }

        // Loop finished cleanly: studentize on the loop's own mean/σ, then
        // fold + publish the ranking
        double loop_mean = 0.0;
        studentize(g_status.runs_total, &loop_mean);
        record_loop(&pairs, loop_mean, loop);   // before the fold clears pairs.l*
        pair_fold_loop(&pairs);
        publish_pair_stats(&pairs);
        if (cumulative) {
            for (int i = 0; i < g_status.runs_total; i++)
                s_zsum[i] += g_status.results[i].z_score;   // Σz per fixed combination
            meas_k++;
            publish_cumulative(s_zsum, g_status.runs_total, meas_k, fm, fe, &z2, nm, mx, euro);
            comparisons = g_status.runs_total;
        } else {
            absorb_loop(carry, &carry_n, low_carry, &low_n, fm, fe, &z2, nm, mx, euro);
            comparisons = g_status.runs_total * g_status.loop_current;
        }
        compute_significance(comparisons);
    }
    goto finalize;

done:
    // Aborted mid-loop: publish whatever was completed so far. Measured
    // entries of the aborted loop sit scattered at results[s_perm[...]], so
    // compact them to the front before using them as a partial prefix.
    pair_fold_loop(&pairs);
    publish_pair_stats(&pairs);
    if (cumulative) {
        if (meas_k > 0) {
            // Complete loops exist: discard the partial loop, publish those
            publish_cumulative(s_zsum, g_status.runs_total, meas_k, fm, fe, &z2, nm, mx, euro);
            comparisons = g_status.runs_total;
        } else if (g_status.runs_completed > 0) {
            // Aborted during the first measurement loop: treat the measured
            // subset as a single sample so partial results are still shown
            int pdone = g_status.runs_completed;
            compact_partial(pdone);
            studentize(pdone, NULL);
            for (int i = 0; i < pdone; i++)
                s_zsum[i] = g_status.results[i].z_score;
            publish_cumulative(s_zsum, pdone, 1, fm, fe, &z2, nm, mx, euro);
            comparisons = pdone;
        }
    } else {
        int pdone = g_status.runs_completed;
        if (pdone > 0 && pdone < g_status.runs_total) {
            compact_partial(pdone);
            studentize(pdone, NULL);
        }
        absorb_loop(carry, &carry_n, low_carry, &low_n, fm, fe, &z2, nm, mx, euro);
        comparisons = g_status.runs_total * (g_status.loop_current > 0 ? g_status.loop_current : 1);
    }
    compute_significance(comparisons);

finalize:
    g_status.elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    g_status.state = g_status.abort_requested ? ELOTTO_ABORTED : ELOTTO_DONE;
    vTaskDelete(NULL);
}
