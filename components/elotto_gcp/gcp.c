#include "gcp.h"
#include "camera.h"

#include <math.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
            ones += __builtin_popcount(w);
        }
        if (!camera_read_word(&w)) return GCP_CAM_FAULT;
        ones += __builtin_popcount(w & 0xFF);

        z_sum += (ones - GCP_SEGMENT_MEAN) / GCP_SEGMENT_SD;

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
