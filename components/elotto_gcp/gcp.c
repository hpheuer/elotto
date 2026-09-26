#include "gcp.h"
#include "esp_timer.h"
#include "camera.h"

#include <math.h>
#include <stdint.h>

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

/* One stream, LSB bits as measured. Seven words per segment, all 32 bits.
 * z is the binomial over the window; h1/h2 are the same bits split at the
 * FRAME-PAIR boundary nearest the middle, so the halves compare two
 * moments in time rather than the top and bottom of one frame. Boundaries come
 * from camera_window_pair_starts(); a boundary is taken at the end of the read
 * block it falls into (<= GCP_READ_SEGS segments late, < 0,2 % of a pair).
 * No pair boundary inside the window -> nseg/2 as before. */
gcp_result_t gcp_zscore_pre(int nseg, bool (*on_yield)(void), double *out,
                            double *out_h1, double *out_h2)
{
    const uint64_t words_per_seg = 7;
    const int n1 = nseg / 2;
    uint64_t ones = 0, mid_ones = 0;
    /* Pair boundaries crossed so far: (segment, ones before it). */
    /* Static, not on the stack: the slave's link task has 6 KB. One caller per
     * node at a time (the measuring task), so no reentrancy to protect. */
    enum { NB = 64 };
    static int      b_seg[NB];
    static uint64_t b_ones[NB];
    static uint32_t starts[NB];
    int      nb = 0;
    uint32_t seen = 1;               /* entry 0 is the window start, not a boundary */
    const int poll = nseg / 4 + 1;
    const int64_t t_read0 = esp_timer_get_time();
    uint32_t buf[GCP_READ_SEGS * 7];

    if (out_h1) *out_h1 = 0.0;
    if (out_h2) *out_h2 = 0.0;

    for (int seg = 0; seg < nseg; ) {
        /* A block ends wherever the per-segment loop acted: after the midpoint
         * segment (n1 - 1) and after every segment with seg % poll == 0. So the
         * half split and the abort polls fall on the same segments, with the
         * same counts, as when the words came one at a time. */
        int end = seg + GCP_READ_SEGS;
        if (end > nseg) end = nseg;
        int ys = (seg + poll - 1) / poll * poll;     /* next polling segment */
        if (ys + 1 < end) end = ys + 1;
        if (n1 > seg && n1 < end) end = n1;

        uint32_t nw = (uint32_t)(end - seg) * 7u;
        if (!camera_read_words(buf, nw)) return GCP_CAM_FAULT;
        uint32_t bo = 0;                             /* <= 112 * 32, no overflow */
        for (uint32_t i = 0; i < nw; i++) bo += cam_popcount32(buf[i]);
        ones += bo;

        int last = end - 1;
        if (last + 1 == n1) mid_ones = ones;

        /* Every pair start at or before the words read so far is a boundary. */
        if (out_h1 || out_h2) {
            uint32_t ns = camera_window_pair_starts(starts, NB);
            uint64_t wpos = (uint64_t)end * words_per_seg;
            while (seen < ns && starts[seen] <= wpos && nb < NB) {
                b_seg[nb]  = end;
                b_ones[nb] = ones;
                nb++;
                seen++;
            }
        }

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

    /* The split: the pair boundary nearest nseg/2, strictly inside. */
    int      cut = n1;
    uint64_t cut_ones = mid_ones;
    int      best = -1;
    for (int i = 0; i < nb; i++) {
        if (b_seg[i] <= 0 || b_seg[i] >= nseg) continue;
        int d = b_seg[i] - n1; if (d < 0) d = -d;
        if (best < 0 || d < best) { best = d; cut = b_seg[i]; cut_ones = b_ones[i]; }
    }
    if (out_h1 && cut > 0)
        *out_h1 = z_from_counts(cut_ones, (uint64_t)cut * words_per_seg);
    if (out_h2 && nseg > cut)
        *out_h2 = z_from_counts(ones - cut_ones,
                                words - (uint64_t)cut * words_per_seg);
    return GCP_OK;
}

gcp_result_t gcp_zscore_raw(int nseg, bool (*on_yield)(void), double *out)
{
    return gcp_zscore_pre(nseg, on_yield, out, NULL, NULL);
}
