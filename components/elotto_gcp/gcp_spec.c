/* ── Spectral entropy of the segment series ────────────────────────────────
 *
 * See include/gcp.h for what this measures and why it is orthogonal to z.
 * This file holds the machinery: a float32 radix-2 FFT, the Welch
 * accumulation, the entropy reduction, and the closed-form null moments.
 *
 * ⚠ NOTHING here may touch the z path. gcp_zscore_raw() feeds this accumulator
 * with the same `ones` it already computed; the z arithmetic in gcp.c is
 * unchanged and stays bit-identical to every z this rig has recorded.
 */
#include "gcp.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "esp_heap_caps.h"

#define SPEC_N   (GCP_SPEC_W / 2)      /* complex points in the packed FFT */

/* ── Buffers ───────────────────────────────────────────────────────────────
 * ⚠ INTERNAL RAM FIRST, PSRAM only as a fallback — and this is a MEASURED
 * requirement, not a preference. The first version put all ~12 KB in PSRAM and
 * the cost was enormous: measured 2026-08-25 in one session with three nodes on
 * the new firmware and one still on the old,
 *
 *   master (new) 5,717 Mbit/s · slave0 3,824 · slave1 3,809 · slave2 (OLD) 5,717
 *   ms_extract      40,2                67,1           67,5              39,3
 *
 * i.e. the entropy channel took 33 % of the slaves' bit rate. The FFT itself
 * cannot explain that — 25 windows of 512 points is ~750 kFLOP against a 1 s
 * run. What explains it is the BUS: the capture buffers and the extraction ring
 * are in PSRAM, and one float store per segment plus the FFT's traffic over the
 * same interface starves the extractor. `ms_extract` is where it shows, which is
 * exactly the signature CLAUDE.md warns about for this task pair.
 *
 * The master was unaffected in that measurement and that is not evidence of
 * anything: it finishes its own run and then waits ~1,3 s for the slave replies,
 * so its extraction task gets the idle bus back. Never judge this change on the
 * master's rate.
 *
 * ~12 KB of internal RAM: 4 KB window + 2 KB accumulator + 6 KB twiddles.
 * ⚠ It is allocated at RUNTIME, not .bss, precisely because internal RAM is
 * nearly full with results[] on the master — a static array of this size would
 * fail the LINK there (see NUM_RUNS in sensor.h). Heap can also fail, and then
 * the accumulator stays disabled and the session runs with z alone: entropy is
 * never allowed to be a precondition for a measurement.
 *
 * ⚠ ONE set of buffers, so this is NOT reentrant. Both firmwares call it from
 * exactly one measurement task, which is the same assumption gcp_zscore_raw()
 * already makes about camera_read_word(). */
static float *s_re, *s_im;             /* SPEC_N each: the packed window       */
static float *s_acc;                   /* GCP_SPEC_BINS: Σ|X_k|² over windows  */
static float *s_twr, *s_twi;           /* SPEC_N/2 each: FFT twiddles          */
static float *s_unr, *s_uni;           /* SPEC_N each: real-unpack twiddles    */
static int    s_alloc_tried;
/* 1 = at least one buffer had to go to PSRAM. Published by /spectest, because
 * a node in that state runs measurably slower than its peers and nothing else
 * would say why. */
static int    s_in_psram;

static bool spec_alloc(void)
{
    if (s_acc) return true;
    if (s_alloc_tried) return false;
    s_alloc_tried = 1;

    /* Internal first for every buffer; PSRAM only if internal is exhausted, and
     * a node that lands there will show it as a lower cam_mbit than its peers. */
    #define SPEC_ALLOC(n)                                                     \
        ({ void *_p = heap_caps_malloc((n), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); \
           if (!_p) { _p = heap_caps_malloc((n), MALLOC_CAP_SPIRAM);          \
                      s_in_psram = 1; }                                       \
           _p; })
    s_re  = SPEC_ALLOC(sizeof(float) * SPEC_N);
    s_im  = SPEC_ALLOC(sizeof(float) * SPEC_N);
    s_acc = SPEC_ALLOC(sizeof(float) * GCP_SPEC_BINS);
    s_twr = SPEC_ALLOC(sizeof(float) * (SPEC_N / 2));
    s_twi = SPEC_ALLOC(sizeof(float) * (SPEC_N / 2));
    s_unr = SPEC_ALLOC(sizeof(float) * SPEC_N);
    s_uni = SPEC_ALLOC(sizeof(float) * SPEC_N);
    #undef SPEC_ALLOC
    if (!s_re || !s_im || !s_acc || !s_twr || !s_twi || !s_unr || !s_uni) {
        free(s_re);  free(s_im);  free(s_acc);
        free(s_twr); free(s_twi); free(s_unr); free(s_uni);
        s_re = s_im = s_acc = s_twr = s_twi = s_unr = s_uni = NULL;
        return false;
    }

    /* Tabulated rather than recurrent. A recurrence would accumulate ~256
     * float32 rounding steps per stage; the error is the same on every node and
     * every window, so it would land in H as a constant that block centring
     * removes anyway — but a table costs 6 KB once and removes the question.
     * Built in double, stored in float. */
    for (int j = 0; j < SPEC_N / 2; j++) {
        double a = -2.0 * M_PI * (double)j / (double)SPEC_N;
        s_twr[j] = (float)cos(a);
        s_twi[j] = (float)sin(a);
    }
    for (int k = 0; k < SPEC_N; k++) {
        double a = -M_PI * (double)k / (double)SPEC_N;
        s_unr[k] = (float)cos(a);
        s_uni[k] = (float)sin(a);
    }
    return true;
}

/* In-place iterative radix-2 complex FFT, float32, n = SPEC_N. */
static void fft_c(float *re, float *im)
{
    const int n = SPEC_N;
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        int step = n / len;               /* index stride into the twiddle table */
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; j++) {
                float wr = s_twr[j * step], wi = s_twi[j * step];
                float xr = re[i + j + half], xi = im[i + j + half];
                float vr = xr * wr - xi * wi;
                float vi = xr * wi + xi * wr;
                float ur = re[i + j], ui = im[i + j];
                re[i + j]        = ur + vr;  im[i + j]        = ui + vi;
                re[i + j + half] = ur - vr;  im[i + j + half] = ui - vi;
            }
        }
    }
}

/* Fold one filled window into s_acc.
 *
 * The window holds GCP_SPEC_W real samples packed as SPEC_N complex points
 * (even → re, odd → im). After one SPEC_N-point complex FFT the real spectrum
 * is recovered as
 *
 *   E[k] = (Z[k] + conj(Z[N-k]))/2       O[k] = (Z[k] - conj(Z[N-k]))/(2i)
 *   X[k] = E[k] + e^{-iπk/N} · O[k]
 *
 * We keep |X[k]|² for k = 1 … N-1 only: k = 0 is the DC bin, which IS z's
 * channel (Σ(ones-100) is exactly the 0-th Fourier component), and k = N is
 * Nyquist, which is real-valued and therefore χ²₁ where every other bin is
 * χ²₂ — it does not belong in a set the null treats as iid. */
static void spec_fold(void)
{
    fft_c(s_re, s_im);

    for (int k = 1; k < SPEC_N; k++) {
        float ar = s_re[k],          ai = s_im[k];
        float br = s_re[SPEC_N - k], bi = -s_im[SPEC_N - k];   /* conj(Z[N-k]) */

        float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        /* O = (Z[k] - conj(Z[N-k])) / (2i);  dividing by i is (x+iy) → (y-ix) */
        float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
        float or_ =  di, oi_ = -dr;

        float tr = s_unr[k], ti = s_uni[k];
        float xr = er + (or_ * tr - oi_ * ti);
        float xi = ei + (or_ * ti + oi_ * tr);

        s_acc[k - 1] += xr * xr + xi * xi;
    }
}

/* ── Reference path, for gcp_spec_selftest() ───────────────────────────────
 * A naive O(W²) DFT of the same window. Compiled in permanently as the
 * DEFINITION of what spec_fold() must produce, exactly as cam_extract_ref()
 * is for the extractor. A wrong-but-plausible H looks exactly like a result. */
static void spec_ref_power(const float *x, double *out_p)
{
    for (int k = 1; k < SPEC_N; k++) {
        double sr = 0.0, si = 0.0;
        for (int n = 0; n < GCP_SPEC_W; n++) {
            double a = -2.0 * M_PI * (double)k * (double)n / (double)GCP_SPEC_W;
            sr += (double)x[n] * cos(a);
            si += (double)x[n] * sin(a);
        }
        out_p[k - 1] = sr * sr + si * si;
    }
}

void gcp_spec_begin(gcp_spec_t *sp)
{
    if (!sp) return;
    memset(sp, 0, sizeof(*sp));
    if (!spec_alloc()) return;
    sp->ok = true;
    memset(s_acc, 0, sizeof(float) * GCP_SPEC_BINS);
}

void gcp_spec_push(gcp_spec_t *sp, int ones)
{
    if (!sp || !sp->ok) return;
    /* Centred in integers, like the z path: ones ∈ [0,200] so ones-100 is exact
     * in float. The per-window mean is NOT removed — dropping bin 0 does that
     * for us, and removing it here would only reintroduce z into H. */
    int i = sp->fill;
    if (i & 1) s_im[i >> 1] = (float)(ones - GCP_SEGMENT_MEAN_I);
    else       s_re[i >> 1] = (float)(ones - GCP_SEGMENT_MEAN_I);
    if (++sp->fill == GCP_SPEC_W) {
        spec_fold();
        sp->fill = 0;
        sp->m++;
    }
}

bool gcp_spec_finish(gcp_spec_t *sp, double *out_h, int *out_m)
{
    if (!sp || !sp->ok || sp->m < GCP_SPEC_MIN_WIN) return false;

    /* Reduction in double. 511 terms at ~1/511·ln(511) each, against a σ of
     * ~2e-4 in the normalised value — float32 here would be visible. */
    double s = 0.0;
    for (int k = 0; k < GCP_SPEC_BINS; k++) s += (double)s_acc[k];
    if (!(s > 0.0)) return false;

    double h = 0.0;
    for (int k = 0; k < GCP_SPEC_BINS; k++) {
        double p = (double)s_acc[k] / s;
        if (p > 0.0) h -= p * log(p);
    }
    if (out_h) *out_h = h / log((double)GCP_SPEC_BINS);
    if (out_m) *out_m = sp->m;
    return true;
}

/* ── The null ──────────────────────────────────────────────────────────────
 * Under H₀ the segment series is white, so each averaged periodogram bin is
 * Gamma(M,1) up to scale and the K normalised bins are exchangeable. Writing
 * H = ln S − (1/S)Σ G ln G and linearising:
 *
 *   E[H]/lnK   = 1 − (ψ(M+1) − ln M)/lnK            deficit ≈ 1/(2M)
 *   Var(H)     = Var(Z)/(K·M²),  Z = (1+ψ(M+1))(G−M) − (G lnG − Mψ(M+1))
 *
 * with E[G lnG] = Mψ(M+1), E[G² lnG] = M(M+1)ψ(M+2) and
 * E[G²ln²G] = M(M+1)[ψ(M+2)²+ψ'(M+2)]. Var(Z) runs from 0,2899 at M=1 to 0,5
 * as M→∞; at the two window lengths this rig offers it is 0,490 (M=25, ?run=1)
 * and 0,498 (M=127, ?run=5).
 *
 * ACCURACY, measured against Monte Carlo (3000 draws at M=25, 600 at M=127,
 * K=511, exact Gamma(M,1) bins):
 *   M=25   mean 0,99682236 vs 0,99681438 closed form  (8e-6, i.e. 0,04 σ)
 *   M=127  mean 0,99937068 vs 0,99936953              (1,2e-6)
 *   σ      ratio MC/theory 0,984 at M=25 and 0,945 at M=127
 * The mean is exact for every purpose here. The σ from the linearisation runs
 * a few percent HIGH, i.e. z_h is a few percent conservative. That is harmless
 * by construction and worth writing down rather than rediscovering: M is fixed
 * for a whole session, so the error is a CONSTANT SCALE on every item's z_h —
 * it cannot reorder anything, and its only effect is that the entropy half of
 * the ranking key carries ~5 % less weight than the nominal ?went= says. It is
 * NOT a reason to treat z_h as a p-value; the reason for that is below.
 *
 * ⚠ Computed from the run's ACTUAL M, never from a constant. M follows the
 * segment count, which follows ?run= AND the camera's delivered rate, so a
 * hard-coded pair would silently mean something different at a different window
 * — the same defect gcp_z_per_bias() exists to prevent for the bias bar.
 *
 * ⚠ This is the IDEAL null and the instrument does not exactly meet it:
 * consecutive segments come from the same frame pair, so a per-frame offset
 * puts a real line in the spectrum and the MEASURED H₀ sits a little below the
 * value here. That is a constant per node per operating point, which is what
 * block centring removes — and it is precisely why z_h must NOT feed the
 * Bonferroni line or the null gates. It ranks; z tests. */
static double psi_f(double x)
{
    double r = 0.0;
    while (x < 8.0) { r -= 1.0 / x; x += 1.0; }
    double f = 1.0 / (x * x);
    return r + log(x) - 0.5 / x
           - f * (1.0/12.0 - f * (1.0/120.0 - f * (1.0/252.0 - f * (1.0/240.0))));
}

static double psi1_f(double x)
{
    double r = 0.0;
    while (x < 8.0) { r += 1.0 / (x * x); x += 1.0; }
    double f = 1.0 / (x * x);
    return r + (1.0 / x) * (1.0 + 0.5 / x
                            + f * (1.0/6.0 - f * (1.0/30.0 - f * (1.0/42.0))));
}

bool gcp_spec_null(int m, double *out_h0, double *out_sd)
{
    if (m < GCP_SPEC_MIN_WIN) return false;
    const double M   = (double)m;
    const double K   = (double)GCP_SPEC_BINS;
    const double lnK = log(K);

    double p1 = psi_f(M + 1.0);
    double p2 = psi_f(M + 2.0);
    double t2 = psi1_f(M + 2.0);

    double nu  = p1;                                   /* E[G lnG]/M            */
    double cov = M * (M + 1.0) * p2 - M * M * p1;      /* Cov(G, G lnG)         */
    double vgl = M * (M + 1.0) * (p2 * p2 + t2) - M * M * p1 * p1;   /* Var(GlnG) */
    double vz  = (1.0 + nu) * (1.0 + nu) * M - 2.0 * (1.0 + nu) * cov + vgl;
    if (vz < 0.0) vz = 0.0;      /* the three terms nearly cancel; guard the tail */

    if (out_h0) *out_h0 = 1.0 - (p1 - log(M)) / lnK;
    if (out_sd) *out_sd = sqrt(vz / (K * M * M)) / lnK;
    return true;
}

bool gcp_spec_z(double h_norm, int m, double *out_z)
{
    double h0, sd;
    if (!gcp_spec_null(m, &h0, &sd) || !(sd > 0.0)) return false;
    if (out_z) *out_z = (h_norm - h0) / sd;
    return true;
}

/* ── Self-test ─────────────────────────────────────────────────────────────
 * The packed real FFT against the reference DFT on this node's own silicon,
 * over a window of pseudo-random segment counts. Returns the worst relative
 * error over the 511 published bins. Served by GET /spectest.
 *
 * The bar is loose on purpose: the fast path is float32 and the reference is
 * double, so agreement to ~1e-4 relative is the most that can be asked. What
 * this catches is the failure that matters — a wrong unpack, a swapped
 * conjugate, an off-by-one in the bin range — which shows up at O(1), not at
 * 1e-4. */
/* True when any working buffer had to fall back to PSRAM — see the buffer note
 * at the top of this file. A node in that state loses about a third of its bit
 * rate, so it must be visible rather than merely slow. */
bool gcp_spec_in_psram(void) { return s_in_psram != 0; }

bool gcp_spec_selftest(double *out_worst)
{
    if (!spec_alloc()) return false;

    float *x = malloc(sizeof(float) * GCP_SPEC_W);
    double *p = malloc(sizeof(double) * GCP_SPEC_BINS);
    if (!x || !p) { free(x); free(p); return false; }

    uint32_t st = 0x1234567u;
    for (int n = 0; n < GCP_SPEC_W; n++) {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        /* Binomial-ish: 200 coin flips would be the real thing, but any
         * broad-spectrum window exercises the same arithmetic. */
        int ones = 60 + (int)(st % 81u);
        x[n] = (float)(ones - GCP_SEGMENT_MEAN_I);
    }

    memset(s_acc, 0, sizeof(float) * GCP_SPEC_BINS);
    for (int n = 0; n < GCP_SPEC_W; n++) {
        if (n & 1) s_im[n >> 1] = x[n];
        else       s_re[n >> 1] = x[n];
    }
    spec_fold();
    spec_ref_power(x, p);

    double worst = 0.0;
    for (int k = 0; k < GCP_SPEC_BINS; k++) {
        double d = fabs((double)s_acc[k] - p[k]);
        double s = fabs(p[k]);
        double rel = (s > 1e-9) ? d / s : d;
        if (rel > worst) worst = rel;
    }
    free(x); free(p);
    if (out_worst) *out_worst = worst;
    return worst < 1e-3;
}
