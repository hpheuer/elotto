#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "extract.h"

/* ── Reference: the original byte-at-a-time loop ───────────────────────────
 * Moved verbatim out of camera.c's diff_and_extract(). It is not dead code —
 * it is the definition of what the fast path has to reproduce, and the
 * self-test runs it on the target every time it is asked. */
/* Close a pre-fold mini-run and start the next. Integer only: the extractor
 * never calls into soft-float, so the sums go out as sums and camera.c reduces
 * them to a σ at publish time. Called once per 3200 pixels, i.e. 200 times per
 * frame against 160000 bulk iterations — it is not on the hot path in any
 * meaningful sense. */
/* popcount of a 4-bit value. One byte load against seven ALU ops, and the
 * table is 16 bytes — it lives in the same cache line for the life of the
 * loop. Used by the runs channel in both extractors. */
static const uint8_t s_pc4[16] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 };

static inline void raw_tick(cam_raw_t *raw)
{
    if (raw->run_bits < CAM_RAW_MINIRUN_BITS) return;
    uint64_t o = raw->run_ones;
    raw->mr_sum   += o;
    raw->mr_sumsq += o * o;
    raw->mr_n++;
    raw->run_ones = 0;
    raw->run_bits = 0;
}

void cam_extract_ref(const uint8_t *a, const uint8_t *b, uint32_t n,
                     cam_pack_t *st, cam_emit_fn emit, void *ctx,
                     uint32_t *out_zeros, uint32_t *out_any, uint32_t *out_psum,
                     cam_raw_t *raw)
{
    uint32_t acc = st->bitacc;
    int      accn = st->bitacc_n;
    uint32_t zeros = 0, psum = 0;
    uint8_t  any = 0;
    uint32_t rw = 0;

    for (uint32_t i = 0; i < n; i++) {
        psum += a[i];
        uint8_t d = (uint8_t)(b[i] - a[i]);
        any |= d;
        if (d == 0) zeros++;
        uint32_t bit = d & 1u;
        if (raw) { raw->ones += bit; raw->run_ones += bit; rw += bit;
                   raw->bits++; raw->run_bits++; raw_tick(raw);
                   if (raw->want_runs) {
                       if (raw->have_prev) raw->trans += (bit ^ raw->prev);
                       raw->prev = bit; raw->have_prev = true;
                   } }
        acc = (acc << 1) | bit;
        if (++accn == 32) { emit(acc, rw, ctx); acc = 0; accn = 0; rw = 0; }
    }

    st->bitacc = acc; st->bitacc_n = accn;
    if (out_zeros) *out_zeros += zeros;
    if (out_any)   *out_any   |= any;
    if (out_psum)  *out_psum  += psum;
}

/* ── Fast: 4 pixels per 32-bit XOR ─────────────────────────────────────────
 *
 * Three tricks, all exact:
 *
 * 1. LSB(b-a) == LSB(a)^LSB(b), so the subtraction disappears (extract.h).
 *
 * 2. Gathering the four byte-LSBs of a word into a nibble, in pixel order and
 *    MSB-first, is one mask and one multiply:
 *        nib = ((x & 0x01010101) * 0x08040201) >> 24
 *    The multiplier is 2^27+2^18+2^9+2^0, so byte k's LSB lands in bit 27-k and
 *    nothing else reaches bits 24..27. RV32 here has no PEXT to do it directly.
 *
 * 3. Counting ZERO bytes must be exact, because zero_diff_frac is published.
 *    ⚠ The usual haszero trick (x-0x01010101) & ~x & 0x80808080 is NOT exact
 *    for counting: a zero byte borrows into the next byte and flags it too
 *    (byte pair 0x01,0x00 reports two zeros). This is the borrow-free form —
 *    adding 0x7F to a 7-bit value cannot carry out of its byte — which is exact
 *    per byte:
 *        ~( ((x & 0x7F7F7F7F) + 0x7F7F7F7F) | x | 0x7F7F7F7F )
 *    leaves 0x80 in a byte position iff that byte is zero, and nowhere else. */
#define LSB_MASK   0x01010101u
#define GATHER_MUL 0x08040201u
#define LOW7       0x7F7F7F7Fu

void cam_extract_fast(const uint8_t *a, const uint8_t *b, uint32_t n,
                      cam_pack_t *st, cam_emit_fn emit, void *ctx,
                      uint32_t *out_zeros, uint32_t *out_any, uint32_t *out_psum,
                      cam_raw_t *raw)
{
    uint32_t acc = st->bitacc;
    int      accn = st->bitacc_n;
    uint32_t zeros = 0, orx = 0, psum = 0;
    const int k = 4;                     /* one bit per pixel */
    uint32_t i = 0;

    /* ── The pre-fold monitor, entirely in LOCALS for the duration ───────
     * It used to run out of *raw directly, and that cost far more than the
     * arithmetic in it: emit() is an INDIRECT call, so the compiler had to
     * assume every word emitted might write through `raw` and reloaded all
     * seven counters afterwards. Four of them are uint64_t, which on RV32 is a
     * two-word load-modify-store with a carry each — the bulk loop was paying
     * roughly twenty-five instructions per four pixels for bookkeeping worth
     * about six, and /camtest measured the whole monitor at +28,6 % of the
     * extraction loop (ns_raw 68,5 against ns_fast 53,3). After this change,
     * on hardware: 59,18 against 54,04, i.e. +9,5 %.
     *
     * Locals are invisible to emit() by construction, so the whole monitor
     * lives in registers and is written back once, at the end.
     *
     * ⚠ `ones` and `bits` are uint32 here on purpose: both are bounded by `n`,
     * one frame is 640000 pixels, and the widening happens at write-back. The
     * mini-run sums stay 64-bit — mr_sumsq grows by up to 3200² per term.
     * ⚠ `bits` counts PIXELS CONSUMED and every path below consumes all of
     * them, so it is not incremented in any loop: it is `n`, added at the end.
     * That alone removed a 64-bit accumulate per four pixels. */
    uint32_t r_word     = 0;             /* ones since the last emit         */
    uint32_t r_ones     = 0;             /* ones in the words already emitted */
    uint32_t r_run_ones = raw ? raw->run_ones : 0u;
    uint32_t r_run_bits = raw ? raw->run_bits : 0u;
    uint32_t r_mr_n     = 0;
    uint64_t r_mr_sum   = 0, r_mr_sumsq = 0;
    /* Runs channel, same locals-only treatment as the rest of the monitor.
     * `r_tmask` folds have_prev into the transition mask so the "no previous
     * bit yet" case costs a move rather than a test: 0x7 drops the leading
     * transition of the very first nibble, and every path sets it to 0xF the
     * moment it has consumed a bit. */
    const bool wr       = raw && raw->want_runs;
    uint32_t r_trans    = 0;
    uint32_t r_prev     = wr ? raw->prev : 0u;
    uint32_t r_tmask    = (wr && raw->have_prev) ? 0xFu : 0x7u;

    /* raw_tick() against the locals. Same test, same moment, same result — the
     * self-test compares mr_n, mr_sum, mr_sumsq, run_ones and run_bits against
     * the reference path on every case, so a divergence here cannot ship. */
#define RAW_TICK()                                                      \
    do {                                                                \
        if (r_run_bits >= CAM_RAW_MINIRUN_BITS) {                       \
            uint64_t o_ = r_run_ones;                                   \
            r_mr_sum += o_; r_mr_sumsq += o_ * o_; r_mr_n++;            \
            r_run_ones = 0; r_run_bits = 0;                             \
        }                                                               \
    } while (0)
#define RAW_EMIT()  do { emit(acc, r_word, ctx);                        \
                         r_ones += r_word; r_word = 0; } while (0)

    /* The bulk path emits 4 bits at a time, so it can only run from a state
     * where that cannot straddle a word boundary. A byte-wise prologue walks
     * into that state; in practice the frame size is a multiple of 64 and the
     * state is already clean, so the prologue runs zero times. */
    /* BOTH pointers must be 4-aligned, not just a: the bulk loads from each.
     * If the two frames happen to differ in alignment mod 4 this condition
     * never clears and the whole frame goes byte-wise -- correct, just slow.
     * The V4L2 buffers are DMA-aligned, so it does not happen here. */
    while (i < n && ((accn % k) != 0 ||
                     (((uintptr_t)(a + i) | (uintptr_t)(b + i)) & 3u))) {
        psum += a[i];
        uint8_t d = (uint8_t)(b[i] - a[i]);
        orx |= d;
        if (d == 0) zeros++;
        uint32_t bit = d & 1u;
        if (raw) { r_word += bit; r_run_ones += bit; r_run_bits++; RAW_TICK(); }
        if (wr) { r_trans += (r_tmask >> 3) & (bit ^ r_prev);
                  r_prev = bit; r_tmask = 0xFu; }
        acc = (acc << 1) | bit;
        if (++accn == 32) { RAW_EMIT(); acc = 0; accn = 0; }
        i++;
    }

    for (; i + 4 <= n; i += 4) {
        uint32_t aw, bw;
        memcpy(&aw, a + i, 4);           // no alignment assumption; compiles to lw
        memcpy(&bw, b + i, 4);
        uint32_t x = aw ^ bw;
        orx |= x;

        /* Byte sum of `aw` without unpacking: two 16-bit lanes, then fold.
         * Six ops for four pixels, against ~7 ms per pair for the separate
         * strided pass this replaces — CPU that is really saved, though it buys
         * no idle bit rate; see extract.h. Max 640000*255 = 1,6e8, so a uint32
         * accumulator cannot overflow within one frame. */
        uint32_t ps = (aw & 0x00FF00FFu) + ((aw >> 8) & 0x00FF00FFu);
        psum += (ps & 0xFFFFu) + (ps >> 16);

        /* zt carries 0x80 in each zero byte and nothing else, so shifting the
         * flags down to bit 0 of their byte and multiplying by 0x01010101 sums
         * them into the top byte -- three cheap ops instead of a libgcc call. */
        uint32_t zt = ~(((x & LOW7) + LOW7) | x | LOW7);
        zeros += ((zt >> 7) * 0x01010101u) >> 24;

        /* Pre-fold ones for these 4 pixels. `x & LSB_MASK` is already needed
         * for the gather below, and multiplying a 0/1-per-byte value by
         * 0x01010101 sums the four bytes into the top byte — the same trick
         * `zeros` uses, max 4 so it cannot carry out. Three ops, and it reads
         * no memory the loop was not already holding in registers. */
        if (raw) {
            uint32_t ro = ((x & LSB_MASK) * 0x01010101u) >> 24;
            r_word += ro; r_run_ones += ro; r_run_bits += 4;
            RAW_TICK();
        }

        uint32_t nib = ((x & LSB_MASK) * GATHER_MUL) >> 24;   // p0 p1 p2 p3

        /* Transitions among prev,p0,p1,p2,p3. Stack the carried bit on top of
         * the nibble and XOR against a one-bit right shift: bit 3 becomes
         * prev^p0, bits 2..0 the three internal pairs. Masking with r_tmask
         * drops bit 3 on the very first nibble of a window. */
        if (wr) {
            uint32_t w = (r_prev << 4) | nib;
            r_trans += s_pc4[(w ^ (w >> 1)) & r_tmask];
            r_prev = nib & 1u;                              /* p3 */
            r_tmask = 0xFu;
        }

        uint32_t v = nib;
        acc = (acc << k) | v;
        accn += k;
        if (accn == 32) { RAW_EMIT(); acc = 0; accn = 0; }
    }

    for (; i < n; i++) {                 // tail
        psum += a[i];
        uint8_t d = (uint8_t)(b[i] - a[i]);
        orx |= d;
        if (d == 0) zeros++;
        uint32_t bit = d & 1u;
        if (raw) { r_word += bit; r_run_ones += bit; r_run_bits++; RAW_TICK(); }
        if (wr) { r_trans += (r_tmask >> 3) & (bit ^ r_prev);
                  r_prev = bit; r_tmask = 0xFu; }
        acc = (acc << 1) | bit;
        if (++accn == 32) { RAW_EMIT(); acc = 0; accn = 0; }
    }

    if (raw) {
        raw->ones     += r_ones + r_word;   /* r_word: the unemitted partial */
        raw->bits     += n;                 /* every path consumed all of it */
        raw->run_ones  = r_run_ones;
        raw->run_bits  = r_run_bits;
        raw->mr_n     += r_mr_n;
        raw->mr_sum   += r_mr_sum;
        raw->mr_sumsq += r_mr_sumsq;
        /* Only when armed: with the channel off r_prev/r_tmask were never
         * advanced and writing them back would leave `prev` describing a bit
         * that is now thousands of pixels behind. r_tmask == 0xF is exactly
         * "have_prev was already set, or this call consumed at least one
         * pixel", so it doubles as the new have_prev. */
        if (wr) {
            raw->trans    += r_trans;
            raw->prev      = r_prev;
            raw->have_prev = (r_tmask == 0xFu);
        }
    }

    st->bitacc = acc; st->bitacc_n = accn;
    if (out_zeros) *out_zeros += zeros;
    if (out_any)   *out_any   |= orx;
    if (out_psum)  *out_psum  += psum;
}
#undef RAW_TICK
#undef RAW_EMIT

/* ── On-target self-test and micro-benchmark ───────────────────────────────
 * Runs on the node, in the binary that will do the measuring, because that is
 * the only place the claim actually has to hold. A host test would prove
 * something about a different compiler and a different memory system.
 *
 * ⚠ The buffers are the CALLER'S frame size, not a fixed 256 KB. Larger than
 * the L2 cache was never the whole requirement: the number this produces is
 * used to price the live pair loop, so it has to run on the live geometry --
 * 2x640000 B, 64-byte aligned like the DMA buffers -- or the comparison is
 * between two different memory systems. Two micro-optimisations were predicted
 * against the 256 KB figure and both measured 0,0 %.
 *
 * BENCH_MIN/MAX only bound what a caller may ask for; nothing here assumes a
 * particular size. */
#define BENCH_MIN     (64u * 1024u)
#define BENCH_MAX     (2048u * 1024u)
#define BENCH_WORDS_FOR(n)   ((n) / 32u + 8u)

typedef struct { uint32_t *buf; uint32_t n, cap; } collect_t;

static void collect_emit(uint32_t w, uint32_t ro, void *ctx)
{
    collect_t *c = (collect_t *)ctx;
    (void)ro;
    if (c->n < c->cap) c->buf[c->n] = w;
    c->n++;
}

static void count_emit(uint32_t w, uint32_t ro, void *ctx)
{
    (void)w; (void)ro;
    (*(volatile uint32_t *)ctx)++;   /* volatile: must not be optimised away */
}

/* A stand-in for process_word()'s arithmetic, so the benchmark can price the
 * per-word statistics separately from the extraction. Mirrors it: one popcount
 * for the bias, the mini-run sigma accumulation, and the four lag popcounts the
 * autocorrelation gate needs. The ring store is left out on purpose -- it is
 * one PSRAM word and is not what is in question. */
typedef struct {
    uint64_t bits, ones, run_ones, run_bits;
    uint64_t ac[4];
    double   mean, m2;
    int      n;
} statsim_t;

static void stats_emit(uint32_t w, uint32_t ro, void *ctx)
{
    statsim_t *s = (statsim_t *)ctx;
    (void)ro;
    int ones = (int)cam_popcount32(w);
    s->bits += 32; s->ones += ones;
    s->run_ones += ones; s->run_bits += 32;
    if (s->run_bits >= 6400) {
        double z = (s->run_ones - 3200.0) / 40.0;
        s->n++;
        double d = z - s->mean;
        s->mean += d / s->n;
        s->m2 += d * (z - s->mean);
        s->run_ones = 0; s->run_bits = 0;
    }
    for (int L = 1; L <= 4; L++)
        s->ac[L - 1] += (uint64_t)cam_popcount32(w & (w << L));
}

static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return (*s = x);
}

/* One equivalence case: run both implementations over the same bytes from the
 * same starting state and compare the emitted words, the zero count, and the
 * stuck-frame verdict. */
static bool case_equal(const uint8_t *a, const uint8_t *b, uint32_t n,
                       uint32_t *wa, uint32_t *wb, uint32_t cap, uint32_t *out_words,
                       cam_selftest_t *rep)
{
    cam_pack_t s1 = {0}, s2 = {0};
    collect_t c1 = { wa, 0, cap }, c2 = { wb, 0, cap };
    uint32_t z1 = 0, z2 = 0, a1 = 0, a2 = 0, p1 = 0, p2 = 0;
    /* The runs channel is ARMED here: it is gated at runtime, and a self-test
     * that left it off would compare two zeros and call the paths equal. */
    cam_raw_t r1 = {0}, r2 = {0};
    r1.want_runs = true; r2.want_runs = true;

    cam_extract_ref (a, b, n, &s1, collect_emit, &c1, &z1, &a1, &p1, &r1);
    cam_extract_fast(a, b, n, &s2, collect_emit, &c2, &z2, &a2, &p2, &r2);

    if (out_words) *out_words = c1.n;
    rep->ref_z = z1; rep->fast_z = z2;
    if (c1.n != c2.n || c1.n > cap) {
        rep->what = 1; rep->bad_at = c1.n; rep->ref_w = c1.n; rep->fast_w = c2.n;
        return false;
    }
    if (z1 != z2)                    { rep->what = 2; return false; }
    /* The pre-fold monitor is held to the same standard as the emitted bits.
     * The bulk path derives its raw count with the byte-sum trick while the
     * reference adds one bit at a time, so this is a real comparison of two
     * different computations, not a tautology — and it is the only thing that
     * would catch the gather mask and the sum mask being confused. */
    if (r1.ones != r2.ones || r1.bits != r2.bits || r1.mr_n != r2.mr_n ||
        r1.mr_sum != r2.mr_sum || r1.mr_sumsq != r2.mr_sumsq ||
        r1.run_ones != r2.run_ones || r1.run_bits != r2.run_bits) {
        rep->what = 7;
        rep->ref_w  = (uint32_t)r1.ones;  rep->fast_w = (uint32_t)r2.ones;
        rep->bad_at = (uint32_t)r1.bits;
        return false;
    }
    /* The runs channel, held to the same standard. The reference compares one
     * bit against its predecessor; the bulk path derives four transitions at a
     * time out of a nibble and a carried bit, so this is a real comparison of
     * two different computations. The carried state is compared too — a path
     * that counted correctly but left `prev` wrong would drop or invent one
     * transition per frame pair, which no aggregate here would show. */
    if (r1.trans != r2.trans || r1.prev != r2.prev ||
        r1.have_prev != r2.have_prev) {
        rep->what = 9;
        rep->ref_w  = (uint32_t)r1.trans; rep->fast_w = (uint32_t)r2.trans;
        rep->bad_at = (uint32_t)r1.bits;
        return false;
    }
    /* Emitted words ARE the LSBs, so ones in the monitor and popcount of the
     * words must agree. Remainder sits in the packer. */
    {
        uint64_t emitted = 0;
        for (uint32_t i = 0; i < c1.n; i++) emitted += cam_popcount32(wa[i]);
        emitted += cam_popcount32(s1.bitacc);
        if (emitted != r1.ones) {
            rep->what = 8;
            rep->ref_w = (uint32_t)emitted; rep->fast_w = (uint32_t)r1.ones;
            return false;
        }
    }
    /* The pixel sum feeds mean_px (was CAL_MAX_MEAN_PX gate, now publish-only
     * D52), so it is held to the same standard as the bits: exactly equal. */
    if (p1 != p2) { rep->what = 6; rep->ref_w = p1; rep->fast_w = p2; return false; }
    if ((a1 != 0) != (a2 != 0))      { rep->what = 3; return false; }
    if (s1.bitacc != s2.bitacc || s1.bitacc_n != s2.bitacc_n) {
        rep->what = 4; rep->ref_w = s1.bitacc; rep->fast_w = s2.bitacc;
        rep->bad_at = (uint32_t)s1.bitacc_n;
        return false;
    }
    for (uint32_t i = 0; i < c1.n; i++)
        if (wa[i] != wb[i]) {
            rep->what = 5; rep->bad_at = i; rep->ref_w = wa[i]; rep->fast_w = wb[i];
            return false;
        }
    return true;
}

bool cam_extract_selftest(cam_selftest_t *out, uint32_t bytes)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    if (bytes < BENCH_MIN) bytes = BENCH_MIN;
    if (bytes > BENCH_MAX) bytes = BENCH_MAX;
    bytes &= ~63u;                       /* whole cache lines, like a frame */
    const uint32_t BENCH_BYTES = bytes;
    const uint32_t BENCH_WORDS = BENCH_WORDS_FOR(BENCH_BYTES);
    out->bench_bytes = BENCH_BYTES;

    /* 64-byte aligned, matching the V4L2 capture buffers. Unaligned starts would
     * send the fast path through its byte-wise prologue and price a loop the
     * live one never runs. */
    uint8_t  *a  = heap_caps_aligned_alloc(64, BENCH_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t  *b  = heap_caps_aligned_alloc(64, BENCH_BYTES, MALLOC_CAP_SPIRAM);
    uint32_t *wa = heap_caps_aligned_alloc(64, BENCH_WORDS * 4, MALLOC_CAP_SPIRAM);
    uint32_t *wb = heap_caps_aligned_alloc(64, BENCH_WORDS * 4, MALLOC_CAP_SPIRAM);
    if (!a || !b || !wa || !wb) {
        heap_caps_free(a); heap_caps_free(b);
        heap_caps_free(wa); heap_caps_free(wb);
        return false;
    }

    /* Realistic content: a dark frame plus a small independent perturbation, so
     * roughly the measured 8 % of pixels come out with diff == 0 like the real
     * source does. A uniform-random pair would exercise the zero-byte counter
     * far less than the instrument actually does. */
    uint32_t rs = 0x1234567u;
    for (uint32_t i = 0; i < BENCH_BYTES; i++) {
        uint32_t r = xs32(&rs);
        a[i] = (uint8_t)(16 + (r & 7));
        b[i] = (uint8_t)(a[i] + (int)((r >> 8) % 5) - 2);
    }

    /* ── popcount equivalence ──────────────────────────────────────────────
     * Every 32-bit value would be 4,3e9 iterations; this walks the structured
     * cases that break SWAR implementations (each single bit, each byte
     * boundary, all-ones, alternating masks) and then a pseudo-random sweep. */
    out->popcount_ok = true;
    out->popcount_n  = 0;
    {
        uint32_t rs2 = 0xC0FFEEu;
        for (uint32_t t = 0; t < 200000u && out->popcount_ok; t++) {
            uint32_t v;
            if (t < 32)        v = 1u << t;
            else if (t < 64)   v = 0xFFFFFFFFu >> (t - 32);
            else if (t < 96)   v = 0xFFFFFFFFu << (t - 64);
            else if (t == 96)  v = 0u;
            else if (t == 97)  v = 0xFFFFFFFFu;
            else if (t == 98)  v = 0xAAAAAAAAu;
            else if (t == 99)  v = 0x55555555u;
            else               v = xs32(&rs2);
            out->popcount_n++;
            if (cam_popcount32(v) != (uint32_t)__builtin_popcount(v)) {
                out->popcount_ok  = false;
                out->popcount_bad = v;
            }
        }
    }

    int cases = 0, failed = 0;
    uint32_t words = 0, w;

    /* 1: the real shape. */
    cases++; if (!case_equal(a, b, BENCH_BYTES, wa, wb, BENCH_WORDS, &w, out)) failed = cases;
    words = w;
    /* 2: identical frames -- the stuck-frame path, and every diff zero. */
    memcpy(b, a, BENCH_BYTES);
    cases++; if (!failed && !case_equal(a, b, BENCH_BYTES, wa, wb, BENCH_WORDS, &w, out)) failed = cases;
    /* 3: a single differing byte, so the stuck verdict must flip on one pixel. */
    b[BENCH_BYTES / 3] ^= 1u;
    cases++; if (!failed && !case_equal(a, b, BENCH_BYTES, wa, wb, BENCH_WORDS, &w, out)) failed = cases;
    /* 4/5: lengths that are NOT a multiple of 4, to exercise prologue and tail. */
    for (uint32_t i = 0; i < BENCH_BYTES; i++) b[i] = (uint8_t)(a[i] + (xs32(&rs) & 3u) - 1u);
    cases++; if (!failed && !case_equal(a, b, 1023, wa, wb, BENCH_WORDS, &w, out)) failed = cases;
    cases++; if (!failed && !case_equal(a + 1, b + 3, 4095, wa, wb, BENCH_WORDS, &w, out)) failed = cases;

    out->cases = cases;
    out->failed_case = failed;
    out->equal = (failed == 0) && out->popcount_ok;
    out->words = words;

    /* ── Benchmark. Same bytes, same order, three passes each; the median of a
     * three-run min is overkill here because the buffers do not move. */
    const uint32_t N = BENCH_BYTES;
    int64_t t0;
    volatile uint32_t sink = 0;
    cam_pack_t st;
    uint32_t z, an, ps;

    t0 = esp_timer_get_time();
    { uint32_t o = 0;
      for (uint32_t i = 0; i + 4 <= N; i += 4) {
          uint32_t x, y; memcpy(&x, a + i, 4); memcpy(&y, b + i, 4); o |= x ^ y;
      }
      sink = o; }
    out->ns_read = (float)((esp_timer_get_time() - t0) * 1000.0 / N);

    st = (cam_pack_t){0}; z = 0; an = 0; sink = 0;
    t0 = esp_timer_get_time();
    cam_extract_ref(a, b, N, &st, count_emit, (void *)&sink, &z, &an, &ps, NULL);
    out->ns_ref = (float)((esp_timer_get_time() - t0) * 1000.0 / N);

    st = (cam_pack_t){0}; z = 0; an = 0; sink = 0;
    t0 = esp_timer_get_time();
    cam_extract_fast(a, b, N, &st, count_emit, (void *)&sink, &z, &an, &ps, NULL);
    out->ns_fast = (float)((esp_timer_get_time() - t0) * 1000.0 / N);

    /* The pre-fold monitor priced on its own, against ns_fast directly above.
     * It is the only number that says whether the monitor may stay always-on
     * or has to be gated to calibration and idle: the extraction loop is
     * compute-bound under measurement load (D25), so a cost here is a cost in
     * the loaded bit rate, and nowhere is it visible at idle. */
    { cam_raw_t rw; memset(&rw, 0, sizeof(rw));
      st = (cam_pack_t){0}; z = 0; an = 0; sink = 0;
      t0 = esp_timer_get_time();
      cam_extract_fast(a, b, N, &st, count_emit, (void *)&sink, &z, &an, &ps, &rw);
      out->ns_raw = (float)((esp_timer_get_time() - t0) * 1000.0 / N);
      sink = (uint32_t)rw.ones; }

    { statsim_t ss; memset(&ss, 0, sizeof(ss));
      st = (cam_pack_t){0}; z = 0; an = 0;
      t0 = esp_timer_get_time();
      cam_extract_fast(a, b, N, &st, stats_emit, &ss, &z, &an, &ps, NULL);
      out->ns_stats = (float)((esp_timer_get_time() - t0) * 1000.0 / N);
      sink = (uint32_t)ss.ones; }

    (void)sink;
    out->ran = true;
    heap_caps_free(a); heap_caps_free(b); heap_caps_free(wa); heap_caps_free(wb);
    return true;
}
