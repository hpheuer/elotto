#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "camera.h"   /* cam_popcount32 */

/* ── LSB-diff extraction, as two implementations that MUST agree ───────────
 *
 * The bit stream this produces is the measurement. Every z the rig has ever
 * recorded came out of the loop below, so a faster version is only admissible
 * if it is BIT-IDENTICAL — not "equivalent in distribution", identical. Hence
 * two functions and a self-test that compares them on the target, rather than
 * one function and an argument.
 *
 * `cam_extract_ref()` is the original byte-at-a-time loop, moved here unchanged.
 * `cam_extract_fast()` is the word-wise version. It rests on one identity:
 *
 *     LSB(b - a) == LSB(a) ^ LSB(b)
 *
 * because bit 0 of a subtraction never depends on a borrow. The per-pixel
 * subtraction is therefore not needed at all, and 4 pixels can be done with one
 * XOR of two 32-bit loads.
 *
 * Packing order is preserved exactly: bits go in MSB-first in pixel order, and
 * with the XOR fold on, pixel 2k is XORed with pixel 2k+1 (adjacent pixels are
 * different Bayer channels, which is the point of folding them).
 *
 * State persists ACROSS calls (a frame boundary may land mid-word), so the
 * caller owns it. */

typedef struct {
    uint32_t bitacc;        // partial word, MSB-first
    int      bitacc_n;      // bits in bitacc (0..31)
    uint32_t fold_pending;  // first bit of a fold pair
    bool     fold_have;     // a fold pair is half-collected
} cam_pack_t;

/* Called once per completed 32-bit word. */
typedef void (*cam_emit_fn)(uint32_t word, void *ctx);

/* Both return the number of bytes consumed (== n) and report, via the out
 * params, the two frame-level diagnostics the caller publishes:
 *   *out_zeros  += pixels whose diff was 0 (feeds zero_diff_frac)
 *   *out_any    |= non-zero iff ANY pixel differed (feeds the stuck-frame count)
 *   *out_psum   += the sum of every byte of frame `a` (feeds mean_pixel_level)
 *
 * ⚠ The pixel sum rides along HERE rather than in its own pass over the frame.
 * It used to be accumulate_pixel_level(), striding 16 bytes through the first
 * frame — and a stride inside 64-byte cache lines still pulls EVERY line, so
 * 40000 samples cost a full 625 KB of PSRAM traffic, ~7 ms per pair, on top of
 * the two frames the diff already reads. ⚠ The 7 ms is CPU and it is real, but
 * removing it moved the bit rate by 0,0 %: at idle this loop waits on the
 * sensor, so a CPU saving is absorbed in DQBUF (camera_task). It pays under
 * measurement load, where the loop is compute-bound, and nowhere else. Reusing the words the diff has in
 * registers makes it nearly free AND samples every pixel instead of every 16th,
 * so the CAL_MAX_MEAN_PX gate gets a better estimate, not a worse one.
 * ⚠ mean_pixel_level therefore changes slightly in value across this build. It
 * is the same quantity, measured over 16x the samples.
 * ⚠ *out_any is only ever tested against zero. The reference ORs the byte
 * differences and the fast path ORs the XORs; those are different numbers but
 * they are zero on exactly the same frames, which is the whole contract. */
void cam_extract_ref (const uint8_t *a, const uint8_t *b, uint32_t n, bool fold,
                      cam_pack_t *st, cam_emit_fn emit, void *ctx,
                      uint32_t *out_zeros, uint32_t *out_any, uint32_t *out_psum);
void cam_extract_fast(const uint8_t *a, const uint8_t *b, uint32_t n, bool fold,
                      cam_pack_t *st, cam_emit_fn emit, void *ctx,
                      uint32_t *out_zeros, uint32_t *out_any, uint32_t *out_psum);

/* Result of the on-target self-test + micro-benchmark. Times are nanoseconds
 * PER PIXEL, which is the unit that compares against the 2,78 ns budget one
 * cycle costs at 360 MHz. */
typedef struct {
    bool     ran;
    bool     equal;          // every case matched, bit for bit
    int      cases;          // equivalence cases run
    int      failed_case;    // 1-based index of the first mismatch, 0 = none
    /* What exactly diverged, so a failure is diagnosable from the endpoint
     * instead of by guessing and reflashing. `what`: 1 word count, 2 zero
     * count, 3 stuck verdict, 4 leftover packer state, 5 a word. */
    int      what;
    uint32_t bad_at;         // index of the first differing word
    uint32_t ref_w, fast_w;  // and the two values there
    uint32_t ref_z, fast_z;  // zero counts
    uint32_t words;          // words compared in the largest case
    float    ns_read;        // dual-stream PSRAM read floor
    float    ns_ref;         // reference extraction
    float    ns_fast;        // word-wise extraction
    float    ns_stats;       // word-wise + the per-word statistics of process_word
    uint32_t bench_bytes;    // frame size the benchmark actually ran on
    /* cam_popcount32 vs __builtin_popcount over a value sweep on this silicon.
     * It now feeds a z (gcp_zscore_raw), and the word comparison above would
     * not catch a wrong popcount: the extractor's emitted words do not depend
     * on it. A wrong-but-plausible z looks exactly like a result. */
    bool     popcount_ok;
    uint32_t popcount_n;     // values checked
    uint32_t popcount_bad;   // first value that disagreed, if any
} cam_selftest_t;

/* `bytes` is the FRAME SIZE to benchmark, and the caller passes the live one.
 *
 * ⚠ It used to be a fixed 256 KB while the real frames are 640000 B, and the
 * per-pixel cost it reported was then used to price the live loop -- two
 * micro-optimisations were predicted off that number and both measured 0,0 %.
 * A harness that does not run on the geometry it is pricing cannot settle that,
 * so the size is now the caller's, and the value used comes back in
 * `bench_bytes` so a reading can never again be mistaken for the wrong one.
 *
 * Buffers are 64-byte aligned, like the DMA capture buffers, so alignment is
 * not a difference between the two either.
 *
 * Allocates 2*bytes + 2*(bytes/8) of PSRAM for the duration (~1,4 MB at the
 * real frame size). Safe to call while idle only: it is pure computation on its
 * own buffers and touches no camera state. */
bool cam_extract_selftest(cam_selftest_t *out, uint32_t bytes);
