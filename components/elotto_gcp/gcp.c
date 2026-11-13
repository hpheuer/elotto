#include "gcp.h"
#include "esp_timer.h"
#include "camera.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ⚠ cam_popcount32, not __builtin_popcount: these cores have no Zbb, so the
 * builtin is a CALL to __popcountsi2. Identical results by construction, and
 * GET /camtest holds the two against each other on target (`popcount_ok`).
 *
 * ⚠ The SOFT-FLOAT calls per segment below (__floatsidf, __divdf3, __adddf3 —
 * the P4 FPU is single-precision, so every double is emulated) cost more than
 * the popcounts and are DELIBERATELY LEFT ALONE. Replacing the divide with a
 * reciprocal multiply, or summing `ones` and dividing once, is arithmetically
 * equivalent and NOT bit-identical: both would shift the last bits of every z
 * this rig records. See GCP_SEGMENT_SD in gcp.h.
 *
 * The mean subtraction is an integer subtract (ones - GCP_SEGMENT_MEAN_I); the
 * result converts to double exactly because ones ∈ [0,200].
 * ⚠ Bit-identical, so it does NOT split the pooling table. Verify with
 * GET /camtest and by comparing a z against a pre-change run, not by eye. */
static double z_from_counts(uint64_t ones, uint64_t words)
{
    if (words == 0) return 0.0;
    double n = (double)words * 32.0;
    return ((double)ones - n / 2.0) / (sqrt(n) / 2.0);
}

/* Segments per ring read: 112 words, 448 B of the caller's stack (the
 * slave's link task has 6 KB). Read word by word, the reader's own overhead —
 * a mutex take/give and four fences per 32 bits — was the session's clock. */
#define GCP_READ_SEGS 16

/* ── Maurer's universal statistical test (NIST SP 800-22 §2.9), L = 8 ──
 * Over the same bits z counts, in the order they were read: bytes of each
 * word MSB first (the packer's bit order). The first MAU_Q blocks only fill
 * the last-seen table; each later block adds log2(i − last[b]).
 * log2 in Q16 from a table below MAU_LUT_N; a longer distance (≈ e^−16 per
 * block for random bits) is shifted down to the table's top 12 bits. */
#define MAU_L      8
#define MAU_Q      (10u << MAU_L)      /* 2560 initialisation blocks */
#define MAU_E      7.1836656           /* expected f_n, L = 8 */
#define MAU_VAR    3.238               /* variance of one log2 distance, L = 8 */
#define MAU_LUT_N  4096u
static uint32_t s_mau_lut[MAU_LUT_N];  /* round(log2(d)·65536), d ≥ 1 */
static uint32_t s_mau_last[1u << MAU_L];
static uint32_t s_mau_i;               /* blocks seen in this window */
static uint64_t s_mau_sum;             /* Σ log2 distance, Q16 */
static double   s_last_mau = NAN;

double gcp_last_maurer(void) { return s_last_mau; }

static double   s_last_ser = NAN;
double gcp_last_serial(void) { return s_last_ser; }

/* ── Serial statistics of the window's bits ──────────────────────────────
 * z, the halves and Maurer are four numbers; a pattern that moves none of
 * them is gone once the window closes. These two look at the ORDER of the
 * same bits, per window, and go to the node's /camlog — never into a z.
 *
 *  - acz[L-1], L = 1..4: agreement of each bit with the bit L before it, in
 *    read order (MSB of a word first), ACROSS word boundaries. As z against
 *    the agreement rate p² + q² of the window's own ones-fraction p, so the
 *    node's LSB bias does not read as correlation. + = bits repeat. Lag 1 is
 *    the NIST runs statistic with the sign turned round.
 *  - bchi: χ² of the 256 byte values against p^w·q^(8−w) (w = ones in the
 *    byte), as z = (χ² − 254)/√508. Sees byte patterns that are too frequent
 *    without recurring at short distance, which Maurer does not.
 * Both NAN for a window too short to carry them. */
static uint64_t s_ac_dis[4];           /* DISagreeing pairs at lag 1..4 */
static uint32_t s_ac_prev;             /* the word before, for the boundary */
static bool     s_ac_have;
static uint32_t s_bhist[256];          /* filled in mau_words() */

static void ser_begin(void)
{
    memset(s_ac_dis, 0, sizeof(s_ac_dis));
    memset(s_bhist, 0, sizeof(s_bhist));
    s_ac_have = false;
}

static void ser_words(const uint32_t *w, uint32_t n)
{
    uint32_t prev = s_ac_prev;
    uint32_t d1 = 0, d2 = 0, d3 = 0, d4 = 0;       /* <= 112 * 32 per call */
    for (uint32_t k = 0; k < n; k++) {
        uint32_t x = w[k];
        if (s_ac_have) {
            d1 += cam_popcount32(x ^ ((x >> 1) | (prev << 31)));
            d2 += cam_popcount32(x ^ ((x >> 2) | (prev << 30)));
            d3 += cam_popcount32(x ^ ((x >> 3) | (prev << 29)));
            d4 += cam_popcount32(x ^ ((x >> 4) | (prev << 28)));
        } else {                                    /* first word: no bit before it */
            d1 += cam_popcount32((x ^ (x >> 1)) & 0x7FFFFFFFu);
            d2 += cam_popcount32((x ^ (x >> 2)) & 0x3FFFFFFFu);
            d3 += cam_popcount32((x ^ (x >> 3)) & 0x1FFFFFFFu);
            d4 += cam_popcount32((x ^ (x >> 4)) & 0x0FFFFFFFu);
            s_ac_have = true;
        }
        prev = x;
    }
    s_ac_prev = prev;
    s_ac_dis[0] += d1; s_ac_dis[1] += d2; s_ac_dis[2] += d3; s_ac_dis[3] += d4;
}

static void ser_finish(uint64_t ones, uint64_t words)
{
    float  acz[4] = { NAN, NAN, NAN, NAN };
    double bchi = NAN;
    double n = (double)words * 32.0;
    double p = n > 0.0 ? (double)ones / n : 0.0, q = 1.0 - p;
    if (words >= 2 && p > 0.0 && q > 0.0) {
        double P = p * p + q * q;                   /* agreement rate of iid bits */
        for (int L = 1; L <= 4; L++) {
            double nl = n - (double)L;
            double v  = nl * P * (1.0 - P);
            if (v > 0.0)
                acz[L - 1] = (float)(((nl - (double)s_ac_dis[L - 1]) - nl * P) / sqrt(v));
        }
        double nby = (double)words * 4.0;
        if (nby / 256.0 >= 5.0) {                   /* the usual χ² floor per cell */
            double e[9];
            for (int w = 0; w <= 8; w++) e[w] = nby * pow(p, w) * pow(q, 8 - w);
            double chi = 0.0;
            for (uint32_t b = 0; b < 256; b++) {
                double ex = e[cam_popcount32(b)];
                double d  = (double)s_bhist[b] - ex;
                chi += d * d / ex;
            }
            bchi = (chi - 254.0) / sqrt(2.0 * 254.0);
        }
    }
    camera_winlog_set_serial(acz, (float)bchi);
    /* One signed number for the wire: the Stouffer z of the four lags, so the
     * master can centre and combine it per camera exactly as it does Maurer. */
    double sm = 0.0;
    for (int i = 0; i < 4; i++) sm += (double)acz[i];     /* NAN if any is */
    s_last_ser = sm / 2.0;
}

static void mau_begin(void)
{
    if (s_mau_lut[2] == 0)
        for (uint32_t d = 1; d < MAU_LUT_N; d++)
            s_mau_lut[d] = (uint32_t)(log2((double)d) * 65536.0 + 0.5);
    memset(s_mau_last, 0, sizeof(s_mau_last));
    s_mau_i = 0;
    s_mau_sum = 0;
}

static inline uint32_t mau_log2q16(uint32_t d)
{
    if (d < MAU_LUT_N) return s_mau_lut[d];
    uint32_t s = 0;
    while ((d >> s) >= MAU_LUT_N) s++;
    return s_mau_lut[d >> s] + (s << 16);
}

static void mau_words(const uint32_t *w, uint32_t n)
{
    uint32_t i = s_mau_i;
    uint64_t sum = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t x = w[k];
        for (int sh = 24; sh >= 0; sh -= 8) {
            uint32_t b = (x >> sh) & 0xFFu;
            i++;
            if (i > MAU_Q) sum += mau_log2q16(i - s_mau_last[b]);
            s_mau_last[b] = i;
            s_bhist[b]++;
        }
    }
    s_mau_i = i;
    s_mau_sum += sum;
}

/* z = (f_n − E)/σ with f_n = Σ/K over the K test blocks,
 * σ = c·√(Var/K), c = 0,7 − 0,8/L + (4 + 32/L)·K^(−3/L)/15 (NIST).
 * NAN without a test block. */
static double mau_z(void)
{
    if (s_mau_i <= MAU_Q) return NAN;
    double K  = (double)(s_mau_i - MAU_Q);
    double fn = (double)s_mau_sum / 65536.0 / K;
    double c  = 0.7 - 0.8 / MAU_L
              + (4.0 + 32.0 / MAU_L) * pow(K, -3.0 / MAU_L) / 15.0;
    return (fn - MAU_E) / (c * sqrt(MAU_VAR / K));
}

/* One stream, LSB bits as measured. Seven words per segment, all 32 bits;
 * z is the binomial over the window. */
gcp_result_t gcp_zscore_raw(int nseg, bool (*on_yield)(void), double *out)
{
    const uint64_t words_per_seg = 7;
    uint64_t ones = 0;
    const int poll = nseg / 4 + 1;
    const int64_t t_read0 = esp_timer_get_time();
    uint32_t buf[GCP_READ_SEGS * 7];

    s_last_mau = NAN;
    s_last_ser = NAN;
    mau_begin();
    ser_begin();

    for (int seg = 0; seg < nseg; ) {
        /* A block ends after every segment with seg % poll == 0, so the abort
         * polls fall on the same segments as when the words came one at a
         * time. */
        int end = seg + GCP_READ_SEGS;
        if (end > nseg) end = nseg;
        int ys = (seg + poll - 1) / poll * poll;     /* next polling segment */
        if (ys + 1 < end) end = ys + 1;

        uint32_t nw = (uint32_t)(end - seg) * 7u;
        if (!camera_read_words(buf, nw)) return GCP_CAM_FAULT;
        uint32_t bo = 0;                             /* <= 112 * 32, no overflow */
        for (uint32_t i = 0; i < nw; i++) bo += cam_popcount32(buf[i]);
        ones += bo;
        mau_words(buf, nw);
        ser_words(buf, nw);

        int last = end - 1;
        if (last % poll == 0) {
            vTaskDelay(1);
            if (on_yield && !on_yield()) return GCP_ABORTED;
        }
        seg = end;
    }

    camera_note_consumed((uint64_t)nseg * words_per_seg * 32u,
                         esp_timer_get_time() - t_read0);

    uint64_t words = (uint64_t)nseg * words_per_seg;
    *out = z_from_counts(ones, words);
    s_last_mau = mau_z();
    ser_finish(ones, words);
    return GCP_OK;
}
