#include "gcp.h"
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
gcp_result_t gcp_zscore_raw(int nseg, bool (*on_yield)(void), double *out)
{
    /* Hoisted, as the slave's copy already had it: the master recomputed it per
     * segment for the same value. */
    const int poll  = nseg / 4 + 1;
    double    z_sum = 0.0;

    for (int seg = 0; seg < nseg; seg++) {
        uint32_t w;
        int      ones = 0;

        /* 6 * 32 + 8 = 200 bits. Bits are never invented to cover a failed
         * read — the run ends and produces nothing. */
        for (int i = 0; i < 6; i++) {
            if (!camera_read_word(&w)) return GCP_CAM_FAULT;
            ones += (int)cam_popcount32(w);
        }
        if (!camera_read_word(&w)) return GCP_CAM_FAULT;
        ones += (int)cam_popcount32(w & 0xFFu);

        z_sum += (ones - GCP_SEGMENT_MEAN_I) / GCP_SEGMENT_SD;

        /* The camera path already yields inside camera_read_word() whenever it
         * waits on the producer, which is most of the time. This one is for
         * abort latency, not pacing. */
        if (seg % poll == 0) {
            vTaskDelay(1);
            if (on_yield && !on_yield()) return GCP_ABORTED;
        }
    }

    *out = z_sum / sqrt((double)nseg);
    return GCP_OK;
}
