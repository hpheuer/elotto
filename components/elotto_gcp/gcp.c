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
gcp_result_t gcp_zscore_raw(int nseg, bool (*on_yield)(void), double *out)
{
    return gcp_zscore_pre(nseg, on_yield, out, NULL, NULL, NULL);
}

static double pre_z_from_counts(uint64_t ones, uint64_t words, double bits_per_word)
{
    if (words == 0) return 0.0;
    double n = (double)words * bits_per_word;
    return ((double)ones - n / 2.0) / (sqrt(n) / 2.0);
}

/* ⚠ The z line below is UNCHANGED. The pre-fold channel reads the same
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
 * rung it is far worse — 1,69 at mean_px 5, above 10 over-lit. A p-value from
 * it would not be honest.
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
                            double *out_pre, double *out_h1, double *out_h2)
{
    const bool   want_pre = (out_pre != NULL || out_h1 != NULL || out_h2 != NULL)
                         && camera_raw_stream_ok();
    /* Deterministic: with the fold on a 32-bit word is 64 pixels, off it is 32.
     * Read once — it cannot change inside a window (camera.c reads s_xor_fold
     * per frame and calibration clears the packer). */
    const double bits_per_word = camera_get_xor_fold() ? 64.0 : 32.0;
    /* ⚠ raw_words is NOT counted per word. Seven words are consumed per segment
     * and every failure path returns immediately, so the count is exactly
     * 7*seg -- deriving it saves 913.000 uint64 increments in the hot loop at
     * run=5 and is bit-identical, because it is the same integer either way.
     * The same holds for mid_words at the half-window split. */
    uint64_t raw_ones = 0, raw_words = 0;
    uint64_t mid_ones = 0, mid_words = 0;
    const uint64_t words_per_seg = 7;
    const int n1 = nseg / 2;
    if (out_pre) *out_pre = 0.0;
    if (out_h1)  *out_h1  = 0.0;
    if (out_h2)  *out_h2  = 0.0;

    /* Hoisted, as the slave's copy already had it: the master recomputed it per
     * segment for the same value. */
    const int poll  = nseg / 4 + 1;
    double    z_sum = 0.0;

    /* Two timestamps for the whole run, not one per word: the span this loop
     * spends reading is what device performance means, and 913.000 timer reads
     * would cost more than the thing being measured. Reported once at the end
     * to camera_note_consumed(). A run that faults out reports nothing --
     * an aborted span has no meaningful rate. */
    const int64_t t_read0 = esp_timer_get_time();

    for (int seg = 0; seg < nseg; seg++) {
        uint32_t w;
        int      ones = 0;

        /* 6 * 32 + 8 = 200 bits. Bits are never invented to cover a failed
         * read — the run ends and produces nothing. */
        uint32_t ro = 0;
        for (int i = 0; i < 6; i++) {
            if (!camera_read_word_raw(&w, &ro)) return GCP_CAM_FAULT;
            ones += (int)cam_popcount32(w);
            raw_ones += ro;
        }
        if (!camera_read_word_raw(&w, &ro)) return GCP_CAM_FAULT;
        ones += (int)cam_popcount32(w & 0xFFu);
        /* The whole word is consumed and its pixels were measured, even though
         * the folded segment takes only 8 of its bits. The raw channel counts
         * all of them — that is the point of it. */
        raw_ones += ro;

        z_sum += (ones - GCP_SEGMENT_MEAN_I) / GCP_SEGMENT_SD;

        if (seg + 1 == n1) {
            mid_ones  = raw_ones;
            mid_words = (uint64_t)n1 * words_per_seg;
        }

        /* The camera path already yields inside camera_read_word() whenever it
         * waits on the producer, which is most of the time. This one is for
         * abort latency, not pacing. */
        if (seg % poll == 0) {
            vTaskDelay(1);
            if (on_yield && !on_yield()) return GCP_ABORTED;
        }
    }

    /* ⚠ 32 bits per word, NOT bits_per_word. A ring word is 32 bits wide however
     * many pixels went into it, and mbit_per_sec counts the same 32 per word
     * (s_bits_extracted += 32). Counting the 64 pre-fold pixels here instead
     * would make the consumption rate read double the production rate for the
     * same words -- two numbers in the same column of the same table, in
     * different units. The pre-fold coverage is a property of the words, not a
     * second throughput. */
    camera_note_consumed((uint64_t)nseg * words_per_seg * 32u,
                         esp_timer_get_time() - t_read0);

    *out = z_sum / sqrt((double)nseg);

    raw_words = (uint64_t)nseg * words_per_seg;
    if (want_pre && raw_words > 0) {
        if (out_pre)
            *out_pre = pre_z_from_counts(raw_ones, raw_words, bits_per_word);
        if (out_h1 && n1 > 0)
            *out_h1 = pre_z_from_counts(mid_ones, mid_words, bits_per_word);
        if (out_h2 && nseg > n1)
            *out_h2 = pre_z_from_counts(raw_ones - mid_ones,
                                        raw_words - mid_words, bits_per_word);
    }
    return GCP_OK;
}
