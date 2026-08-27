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
    return gcp_zscore_spec(nseg, on_yield, out, NULL);
}

/* ⚠ The z line below is UNCHANGED and must stay so. gcp_spec_push() is handed
 * the `ones` this loop already counted and touches nothing else — adding the
 * second channel does not move a stored z by a bit, and therefore does not
 * split the pooling table for z. Verify with GET /camtest and by comparing a z
 * against a pre-change run, not by eye. */
gcp_result_t gcp_zscore_spec(int nseg, bool (*on_yield)(void), double *out,
                             gcp_spec_t *sp)
{
    return gcp_zscore_pre(nseg, on_yield, out, sp, NULL);
}

/* ⚠ The z line below is STILL UNCHANGED. The pre-fold channel reads the same
 * words through camera_read_word_raw(), which returns exactly what
 * camera_read_word() would plus a side value — the folded stream, the folded z
 * and every stored z are untouched, and /camtest still holds the extractor to
 * the reference.
 *
 * ── What the pre-fold z is ────────────────────────────────────────────────
 * The XOR fold maps a raw bias e to ~2e², so a MEAN-BIAS effect survives the
 * fold multiplied by sqrt(2)*e: at e = 1e-4 that is a suppression of ~7000x.
 * The fold is kept because the unfolded stream has no stable null (D17: the
 * raw bias wanders ~2e-3 over minutes, twenty times a hypothesised effect) —
 * but that trade throws away the very quantity a GCP-style experiment looks
 * for, so the raw stream is now ALSO scored and archived.
 *
 * ⚠ RANKING AND ARCHIVE ONLY. Its null is the ideal one and this array does not
 * meet it: raw sigma sits at 1,06..1,28 on certified rungs where the folded
 * stream is at 0,997..1,001 (measured on all four nodes 2026-08-27; it was
 * quoted as 1,03..1,10 from a single earlier reading). Outside a certified
 * rung it is far worse — 1,69 at mean_px 5, above 10 over-lit. A p-value from it would not be honest, exactly as
 * for the entropy channel, and for the same reason.
 *
 * ⚠ It covers MORE bits than the folded z, not the same ones: a segment pulls
 * seven words and uses only 8 bits of the seventh, while all seven were
 * physically measured. Same window in TIME, more bits in it. That is why the
 * normalisation below is the plain binomial one over the bits actually
 * consumed rather than a segment count — (ones - N/2)/(sqrt(N)/2), which is
 * identical to Sigma(ones-100)/(GCP_SEGMENT_SD*sqrt(N/200)) and needs no
 * fictitious segment total.
 *
 * `out_pre` NULL, or a node without the parallel ring, disables it. */
gcp_result_t gcp_zscore_pre(int nseg, bool (*on_yield)(void), double *out,
                            gcp_spec_t *sp, double *out_pre)
{
    const bool   want_pre = (out_pre != NULL) && camera_raw_stream_ok();
    /* Deterministic: with the fold on a 32-bit word is 64 pixels, off it is 32.
     * Read once — it cannot change inside a window (camera.c reads s_xor_fold
     * per frame and calibration clears the packer). */
    const double bits_per_word = camera_get_xor_fold() ? 64.0 : 32.0;
    uint64_t raw_ones = 0, raw_words = 0;
    if (out_pre) *out_pre = 0.0;

    /* Hoisted, as the slave's copy already had it: the master recomputed it per
     * segment for the same value. */
    const int poll  = nseg / 4 + 1;
    double    z_sum = 0.0;

    for (int seg = 0; seg < nseg; seg++) {
        uint32_t w;
        int      ones = 0;

        /* 6 * 32 + 8 = 200 bits. Bits are never invented to cover a failed
         * read — the run ends and produces nothing. */
        uint32_t ro = 0;
        for (int i = 0; i < 6; i++) {
            if (!camera_read_word_raw(&w, &ro)) return GCP_CAM_FAULT;
            ones += (int)cam_popcount32(w);
            raw_ones += ro; raw_words++;
        }
        if (!camera_read_word_raw(&w, &ro)) return GCP_CAM_FAULT;
        ones += (int)cam_popcount32(w & 0xFFu);
        /* The whole word is consumed and its pixels were measured, even though
         * the folded segment takes only 8 of its bits. The raw channel counts
         * all of them — that is the point of it. */
        raw_ones += ro; raw_words++;

        z_sum += (ones - GCP_SEGMENT_MEAN_I) / GCP_SEGMENT_SD;

        /* The second channel. One float store per segment, one 512-point FFT
         * per 1024 of them. Arithmetic says ~4 MFLOP per 5 s run, i.e. under
         * 1 % of a core — but that is an ESTIMATE and this project does not get
         * to assert it: the cost is paid by the GCP consumer, which outranks the
         * extraction task, so any error comes out of the BIT RATE.
         * ⚠ Prove it with focus_win_ms and ms_extract under load, never at idle
         * — the same rule every other extraction-path change is held to.
         * NULL disables it entirely. */
        gcp_spec_push(sp, ones);

        /* The camera path already yields inside camera_read_word() whenever it
         * waits on the producer, which is most of the time. This one is for
         * abort latency, not pacing. */
        if (seg % poll == 0) {
            vTaskDelay(1);
            if (on_yield && !on_yield()) return GCP_ABORTED;
        }
    }

    *out = z_sum / sqrt((double)nseg);

    if (want_pre && raw_words > 0) {
        double n = (double)raw_words * bits_per_word;     /* raw bits consumed */
        *out_pre = ((double)raw_ones - n / 2.0) / (sqrt(n) / 2.0);
    }
    return GCP_OK;
}
