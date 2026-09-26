#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "camera.h"   /* cam_popcount32 */

/* ── LSB-diff extraction, as two implementations that MUST agree ───────────
 *
 * The bit stream this produces is the measurement, so a faster version is only
 * admissible if it is BIT-IDENTICAL — not "equivalent in distribution",
 * identical. Hence two functions and a self-test that compares them on the
 * target, rather than one function and an argument.
 *
 * `cam_extract_ref()` is the original byte-at-a-time loop.
 * `cam_extract_fast()` is the word-wise version. It rests on one identity:
 *
 *     LSB(b - a) == LSB(a) ^ LSB(b)
 *
 * because bit 0 of a subtraction never depends on a borrow. The per-pixel
 * subtraction is therefore not needed at all, and 4 pixels can be done with one
 * XOR of two 32-bit loads.
 *
 * Packing order is preserved exactly: bits go in MSB-first in pixel order,
 * one bit per pixel. LSB bits as measured.
 *
 * State persists ACROSS calls (a frame boundary may land mid-word), so the
 * caller owns it. */

typedef struct {
    uint32_t bitacc;        // partial word, MSB-first
    int      bitacc_n;      // bits in bitacc (0..31)
} cam_pack_t;

/* Called once per completed 32-bit word. */
/* `raw_ones` is the number of LSB ones among exactly the pixels that
 * produced this word — 0..32, and 0 when the caller passed no cam_raw_t.
 *
 * ⚠ It is a parameter, not a field the callback reads back out of the
 * cam_raw_t. The callback is reached through a function POINTER, so the
 * compiler has to assume every indirect call may write through that struct:
 * the monitor's counters could not stay in registers across an emit. Handing
 * the count over cuts the dependency, and the extractor keeps the whole
 * monitor in locals. */
typedef void (*cam_emit_fn)(uint32_t word, uint32_t raw_ones, void *ctx);

/* Bit-stream monitor. INTEGER-ONLY: the extractor stays free of soft-float.
 * `bits` counts pixels, one LSB each. The emitted words ARE those bits,
 * so ones in the monitor and popcount of the words must agree.
 * NULL disables it at no cost beyond one branch per call. */
#define CAM_RAW_MINIRUN_BITS 3200u   /* == MINIRUN_BITS in camera.c */

typedef struct {
    uint64_t ones;       /* LSB ones                                           */
    uint64_t bits;       /* bits == pixels consumed                            */
    uint32_t run_ones;   /* ones in the mini-run being filled                  */
    uint32_t run_bits;   /* bits in the mini-run being filled                  */
    uint32_t mr_n;       /* completed mini-runs                                */
    uint64_t mr_sum;     /* Σ ones over completed mini-runs                    */
    uint64_t mr_sumsq;   /* Σ ones² — max 3200² per term, uint64 cannot wrap   */
    /* ── The RUNS channel ──────────────────────────────────────────────────
     * `trans` counts adjacent LSB bit pairs that DIFFER, over the whole
     * stream and across call boundaries. The NIST runs statistic wants the
     * number of runs V = trans + 1, and camera.c reduces it to a z at publish
     * time — integer-only in here, like everything else in this struct.
     *
     * Why transitions and not runs directly: a run straddles frame pairs, so a
     * run counter would need the same carried state AND a special case for the
     * very first bit. Transitions need only the previous bit, which is exactly
     * `prev`/`have_prev`, and V = trans + 1 recovers the count in one add.
     *
     * ⚠ `want_runs` gates it because it is NOT free: the bulk loop pays about
     * nine more ops per four pixels, and this loop is compute-bound under
     * measurement load. Armed at a stats reset, never mid-window. */
    uint64_t trans;      /* adjacent LSB bit pairs that differ            */
    uint32_t prev;       /* last LSB bit seen (0/1)                       */
    bool     have_prev;  /* false only before the very first bit of a window   */
    bool     want_runs;  /* arm the transition count; see above                */
} cam_raw_t;

/* Both return the number of bytes consumed (== n) and report, via the out
 * params, the two frame-level diagnostics the caller publishes:
 *   *out_zeros  += pixels whose diff was 0 (feeds zero_diff_frac)
 *   *out_any    |= non-zero iff ANY pixel differed (feeds the stuck-frame count)
 *   *out_psum   += the sum of every byte of frame `a` (feeds mean_pixel_level)
 *
 * ⚠ The pixel sum rides along HERE rather than in its own pass over the frame:
 * it reuses words the diff already holds, so it samples every pixel instead of
 * every 16th and costs no extra PSRAM traffic.
 * ⚠ *out_any is only ever tested against zero. The reference ORs the byte
 * differences and the fast path ORs the XORs; those are different numbers but
 * they are zero on exactly the same frames, which is the whole contract. */
void cam_extract_ref (const uint8_t *a, const uint8_t *b, uint32_t n,
                      cam_pack_t *st, cam_emit_fn emit, void *ctx,
                      uint32_t *out_zeros, uint32_t *out_any, uint32_t *out_psum,
                      cam_raw_t *raw);
void cam_extract_fast(const uint8_t *a, const uint8_t *b, uint32_t n,
                      cam_pack_t *st, cam_emit_fn emit, void *ctx,
                      uint32_t *out_zeros, uint32_t *out_any, uint32_t *out_psum,
                      cam_raw_t *raw);

/* ── RAW10 (IMX219): MIPI packed, 4 pixels in 5 bytes  ──────────────
 * Bytes 0..3 of a group are bits 9..2 of pixels 0..3, byte 4 holds their bits
 * 1..0 (pixel k at bits 2k+1..2k). The stream bit of a pixel is the LSB of its
 * 10-bit diff, i.e. bit 2k of a[4]^b[4] — the same identity as above. Same
 * contract as the RAW8 pair, per PIXEL: n is bytes, n/5 groups are consumed
 * (a remainder is ignored), `*out_zeros` counts pixels whose full 10-bit diff
 * is 0, `*out_psum` sums the high 8 bits of frame `a` (bytes 0..3 of each
 * group, the 0..255 scale of the RAW8 path), `raw->bits` grows by 4 per group.
 * ⚠ Unlike the RAW8 pair the LSB monitor always runs: a NULL `raw` is
 * replaced by a scratch one, so `raw_ones` handed to emit is always the real
 * count. The live path always passes one.
 *
 * `cam_extract_raw10_ref()` is the plain loop — the definition of every IMX219
 * stream. `cam_extract_raw10_fast()` is the word-wise version; /camtest holds
 * the two against each other. */
void cam_extract_raw10_ref (const uint8_t *a, const uint8_t *b, uint32_t n,
                            cam_pack_t *st, cam_emit_fn emit, void *ctx,
                            uint32_t *out_zeros, uint32_t *out_any, uint32_t *out_psum,
                            cam_raw_t *raw);
void cam_extract_raw10_fast(const uint8_t *a, const uint8_t *b, uint32_t n,
                            cam_pack_t *st, cam_emit_fn emit, void *ctx,
                            uint32_t *out_zeros, uint32_t *out_any, uint32_t *out_psum,
                            cam_raw_t *raw);

/* ── BITSCAN: every bit of the frame-pair diff (docs/BITSCAN.md) ──────────
 * Diagnostic only: never on the measurement path, never emits a word. Bit k of
 * the NUMERIC diff d = (b − a) mod 2^nbits, per pixel in stream order.
 * ⚠ For k ≥ 1 that is not bit k of a^b (borrows) — the diff is rebuilt.
 * Bit 0 runs in the same accumulator: it is the baseline the other bits are
 * held against, from the same pixels and the same code.
 * Per bit the same four numbers the sweep takes from the LSB — bias, mini-run
 * dispersion (3200-bit runs), runs statistic, lag-1..4 autocorrelation — plus
 * the same-pixel coincidence with bit 0 (`both0`). Integer-only; the caller
 * reduces. State persists across calls (a pair boundary may land mid-word). */
#define CAM_BS_BITS_MAX 10

typedef struct {
    uint64_t ones, trans;
    uint64_t ac_both1[4], ac_pairs[4];
    uint64_t both0;          /* pixels with bit k AND bit 0 set               */
    uint32_t run_ones, mr_n; /* mini-run being filled (100 words) / completed */
    uint64_t mr_sum, mr_sumsq;
    uint32_t w;              /* word being filled, MSB-first                  */
    uint32_t prev;           /* last bit of the previous word                 */
} cam_bs_bit_t;

typedef struct {
    int      nbits;          /* 10 RAW10, 8 RAW8                              */
    uint32_t wn;             /* bits in the words being filled (0..31)        */
    uint32_t run_words;      /* words in the mini-run being filled            */
    uint64_t words;          /* completed words per bit                       */
    uint64_t pixels, zeros;
    int64_t  d_sum;          /* signed diff (−2^(n−1)..2^(n−1)−1), DN         */
    uint64_t d_sumsq;
    uint32_t pairs;
    cam_bs_bit_t b[CAM_BS_BITS_MAX];
} cam_bitscan_t;

/* One frame pair. `packed_raw10` selects the RAW10 layout (n/5 groups),
 * otherwise RAW8 (one byte per pixel). `s->nbits` must be set by the caller. */
void cam_bitscan_pair(cam_bitscan_t *s, const uint8_t *a, const uint8_t *b,
                      uint32_t n, bool packed_raw10);

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
     * count, 3 stuck verdict, 4 leftover packer state, 5 a word, 6 pixel sum,
     * 7 the LSB monitor, 8 LSB-as-is raw-vs-emitted cross-check,
     * 9 the runs channel (transitions or the carried bit). */
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
    float    ns_raw;         // word-wise extraction WITH the LSB monitor
    /* cam_popcount32 vs __builtin_popcount over a value sweep on this silicon.
     * It now feeds a z (gcp_zscore_raw), and the word comparison above would
     * not catch a wrong popcount: the extractor's emitted words do not depend
     * on it. A wrong-but-plausible z looks exactly like a result. */
    bool     popcount_ok;
    uint32_t popcount_n;     // values checked
    uint32_t popcount_bad;   // first value that disagreed, if any
    /* RAW10 pair, same cases and the same `what` codes as above (10 =
     * the per-word raw-ones count handed to emit). Kept apart from `equal`,
     * which stays the RAW8 verdict. Times are per BYTE of a frame, the unit of
     * ns_read, so ns10_* × frame bytes is the cost of one pair. */
    bool     r10_equal;
    int      r10_cases;
    int      r10_failed_case;
    int      r10_what;
    uint32_t r10_bad_at, r10_ref_w, r10_fast_w;
    float    ns10_ref;       // plain loop, monitor on (as live)
    float    ns10_fast;      // word-wise, monitor on (as live)
    float    ns10_stats;     // word-wise + the process_word stand-in
    /* Cache autoload (hardware prefetch) experiment: the autoload
     * control registers as found, then ns_read and ns10_fast again with
     * autoload sections over the two bench buffers — [0] L2 cache only,
     * [1] L1 DCache only, [2] both — registers restored after each. */
    uint32_t al_l1_ctrl, al_l2_ctrl;
    float    ns_read_al[3];
    float    ns10_fast_al[3];
} cam_selftest_t;

/* `bytes` is the FRAME SIZE to benchmark, and the caller passes the live one.
 *
 * ⚠ The size is the caller's: a harness that does not run on the geometry it
 * is pricing cannot settle the question. The value used comes back in
 * `bench_bytes` so a reading can never be mistaken for the wrong one.
 *
 * Buffers are 64-byte aligned, like the DMA capture buffers, so alignment is
 * not a difference between the two either.
 *
 * Allocates 3*bytes + 2*(bytes/8) of PSRAM for the duration (~6,8 MB at the
 * 2 MB cap an IMX219 frame hits); the third buffer saves `b` across the RAW10
 * cases. Safe to call while idle only: it is pure computation on its own
 * buffers and touches no camera state. */
bool cam_extract_selftest(cam_selftest_t *out, uint32_t bytes);
