#include "gcp.h"
#include "esp_timer.h"
#include "camera.h"

#include <math.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ⚠ cam_popcount32, not __builtin_popcount: these cores have no Zbb, so the
 * builtin is a CALL to __popcountsi2 and this loop made seven of them per
 * segment — ~913.000 per run at run=5. Identical results by construction, and
 * GET /camtest holds the two against each other on target (`popcount_ok`).
 *
 * ⚠ The SOFT-FLOAT calls per segment below (__floatsidf, __divdf3, __adddf3 —
 * the P4 FPU is single-precision, so every double is emulated) cost more than
 * the popcounts and are DELIBERATELY LEFT ALONE. Replacing the divide with a
 * reciprocal multiply, or summing `ones` and dividing once, is arithmetically
 * equivalent and NOT bit-identical: both would shift the last bits of every z
 * this rig records. See GCP_SEGMENT_SD in gcp.h.
 *
 * The mean subtraction is the one that WAS safe to move, and it is taken:
 * ones - GCP_SEGMENT_MEAN_I is an integer subtract, and the result converts to
 * double exactly because ones ∈ [0,200]. That drops __subdf3, one call per
 * segment — ~130.000 per run at ?run=5. It was left out of the popcount change
 * on purpose, so that change could be measured on its own; it is in now.
 * ⚠ Bit-identical, so it does NOT split the pooling table. Verify with
 * GET /camtest and by comparing a z against a pre-change run, not by eye. */
static double z_from_counts(uint64_t ones, uint64_t words)
{
    if (words == 0) return 0.0;
    double n = (double)words * 32.0;
    return ((double)ones - n / 2.0) / (sqrt(n) / 2.0);
}

/* One stream, LSB bits as measured (D65). Seven words per segment, all 32 bits.
 * z is the binomial over the window; h1/h2 are the same bits split at nseg/2. */
gcp_result_t gcp_zscore_pre(int nseg, bool (*on_yield)(void), double *out,
                            double *out_h1, double *out_h2)
{
    const uint64_t words_per_seg = 7;
    const int n1 = nseg / 2;
    uint64_t ones = 0, mid_ones = 0;
    const int poll = nseg / 4 + 1;
    const int64_t t_read0 = esp_timer_get_time();

    if (out_h1) *out_h1 = 0.0;
    if (out_h2) *out_h2 = 0.0;

    for (int seg = 0; seg < nseg; seg++) {
        uint32_t w;
        for (int i = 0; i < 7; i++) {
            if (!camera_read_word(&w)) return GCP_CAM_FAULT;
            ones += (uint64_t)cam_popcount32(w);
        }
        if (seg + 1 == n1) mid_ones = ones;

        if (seg % poll == 0) {
            vTaskDelay(1);
            if (on_yield && !on_yield()) return GCP_ABORTED;
        }
    }

    camera_note_consumed((uint64_t)nseg * words_per_seg * 32u,
                         esp_timer_get_time() - t_read0);

    uint64_t words = (uint64_t)nseg * words_per_seg;
    *out = z_from_counts(ones, words);
    if (out_h1 && n1 > 0)
        *out_h1 = z_from_counts(mid_ones, (uint64_t)n1 * words_per_seg);
    if (out_h2 && nseg > n1)
        *out_h2 = z_from_counts(ones - mid_ones,
                                words - (uint64_t)n1 * words_per_seg);
    return GCP_OK;
}

gcp_result_t gcp_zscore_raw(int nseg, bool (*on_yield)(void), double *out)
{
    return gcp_zscore_pre(nseg, on_yield, out, NULL, NULL);
}
