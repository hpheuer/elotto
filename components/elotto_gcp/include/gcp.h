/* ── The GCP measurement primitive, shared by master and slave ──────────
 *
 * This is the one function that turns camera bits into a z-score, and it is
 * compiled into BOTH firmwares from this single definition — the same argument
 * elotto_link makes for the wire format, but with more at stake.
 *
 * Why it must be shared: the combine is Sum(z_node)/sqrt(k) over nodes that
 * measured the same window. That is only meaningful if every node computed z
 * the same way. Until this component existed the function was duplicated in
 * main/sensor.c and elotto_slave/main/slave.c, arithmetic identical but
 * maintained twice; a change to the segment size, the normalisation, or the
 * yield cadence in one copy would have made the array silently inconsistent.
 * Nothing the project measures would have caught it — a wrong-but-plausible z
 * looks exactly like a result. One definition, so the nodes cannot disagree.
 *
 * The two call sites differed only in abort handling, which is now the
 * `on_yield` parameter rather than a forked copy of the loop.
 */
#pragma once

#include <stdbool.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One segment is 200 bits: six full 32-bit words plus the low 8 of a seventh.
 * For a fair coin that is mean 100 and sd sqrt(200 * 0.25) = sqrt(50).
 *
 * GCP_SEGMENT_SD is written as the literal the two copies both carried, not as
 * sqrt(50.0). It is a deliberate choice: this component was introduced as a
 * pure refactor of validated code, and every z the rig has ever recorded came
 * from this constant. Evaluating it differently could move the last bits of a
 * result, and a change in the numbers must never be an accident of tidying. */
#define GCP_SEGMENT_BITS   200
#define GCP_SEGMENT_MEAN   100.0
#define GCP_SEGMENT_SD     7.07106781

/* The same mean as an int, for the one place the subtraction can be done in
 * integers. ones is in [0,200], so ones - 100 is in [-100,100] and converts to
 * double exactly -- identical to (double)ones - 100.0, which is exact for the
 * same reason. This is the only arithmetic rearrangement in the z path that is
 * bit-identical rather than merely equivalent; see gcp.c. */
#define GCP_SEGMENT_MEAN_I 100

/* What a unit of stream bias is WORTH as a per-run z offset, at this run length.
 *
 * A bias b over `segments` segments moves the ones count by (b - 0,5)*200*nseg
 * and the z by that over GCP_SEGMENT_SD*sqrt(nseg), i.e. the offset grows with
 * sqrt(nseg): the same camera imperfection is 1,5 z at 26087 segments and 3,3 at
 * 130435. Every threshold on a bias therefore has to be converted here rather
 * than written down as a constant, or it silently means something different at
 * a different ?run= -- which is exactly what CAM_CAL_FAIL_BIAS did until
 * 2026-08-19. Shared for the same reason as the rest of this header: master and
 * slave must apply the same bar to their own cameras.
 *
 * Returns 0 for a non-positive segment count, which callers read as "unknown". */
static inline double gcp_z_per_bias(int segments)
{
    if (segments <= 0) return 0.0;
    return (double)GCP_SEGMENT_BITS * sqrt((double)segments) / GCP_SEGMENT_SD;
}

/* Why a run failed. The caller needs to tell these apart: a camera fault is
 * reported to the master and gets the node rebooted, while an abort is the
 * master's own doing and is not a fault at all. Both mean the same thing for
 * the data — NO z is produced. A short run is not a small run: its z would be
 * normalised by a sqrt(segments) it never reached, so a void run yields
 * nothing rather than something biased toward zero. */
typedef enum {
    GCP_OK = 0,      /* *out holds a usable z */
    GCP_CAM_FAULT,   /* the camera stopped delivering part-way through */
    GCP_ABORTED,     /* on_yield() asked to stop */
} gcp_result_t;

/* One run of `nseg` segments, consuming `nseg * 200` bits from the camera.
 *
 * `on_yield` is called after each of the ~4 yields per run and returns false to
 * abandon the run (the slave polls its abort socket there). Pass NULL when
 * there is nothing to poll — the master aborts between runs, not inside one.
 * The yield cadence is a fixed fraction of the run rather than a fixed count,
 * so it stays matched across nodes at every run length: per-run wall time is
 * the max over nodes, so a mismatch would slow every measurement to the
 * slowest device.
 *
 * `*out` is written only on GCP_OK. */
gcp_result_t gcp_zscore_raw(int nseg, bool (*on_yield)(void), double *out);

/* ── The SECOND channel: spectral entropy of the same segment series ───────
 *
 * z is the DC bin. Σ(ones-100) is exactly the 0-th Fourier component of the
 * segment series, so every statistic built from bins 1…K is, under H₀,
 * INDEPENDENT of the z this same run produces: a second measurement out of the
 * same bits, at no extra measurement time.
 *
 * What it measures is not bias — diff+LSB+XOR-fold drive bias to ~4b² — but the
 * SHAPE of the spectrum, i.e. correlation structure at every lag the window
 * covers. That makes it the general form of the gate camera.c already applies
 * at bit lags 1…4 (CAL_AUTOC_TOL): here the scale is the segment, 200 bits, and
 * the reach is one window, GCP_SPEC_W · 200 = 204800 bits ≈ two thirds of an
 * OV5647 frame's folded output. Row noise, FPN residue and frame-boundary
 * structure live in exactly that band and the lag-4 gate cannot see them.
 *
 * ⚠ LOW entropy is the interesting direction (user, 2026-08-25): less spectral
 * disorder than white noise. The ranking key therefore takes -z_h.
 *
 * ⚠ IT RANKS, IT DOES NOT TEST. The closed-form null in gcp_spec_null() is the
 * ideal one and the instrument does not exactly meet it — 1600 consecutive
 * segments share a frame pair, so per-frame structure is a real line and the
 * measured H₀ sits slightly low. That offset is constant per node per operating
 * point, which block centring removes; what it must never do is feed the
 * Bonferroni line or null_flags. Those stay on z alone.
 *
 * Welch, not one long periodogram: the estimator has to work at ?run=1 as well
 * as ?run=5, and averaging M windows of a fixed length keeps the bin count (and
 * therefore the entropy's own scale) the same at both, with only M changing.
 *   ?run=1 → ~26000 segments → M ≈ 25,  σ(H_norm) ≈ 2,0e-4
 *   ?run=5 → ~130000 segments → M ≈ 127, σ(H_norm) ≈ 3,9e-5
 * z_h is standardised against the run's OWN M, so the two window lengths give
 * directly comparable numbers — 5 s is simply the sharper instrument. */
#define GCP_SPEC_W        1024   /* segments per Welch window                  */
#define GCP_SPEC_BINS      511   /* published bins: k = 1 … W/2-1              */
#define GCP_SPEC_MIN_WIN     8   /* fewer windows than this → no entropy value */

/* Accumulator state. Lives in the caller; the working buffers are one shared
 * allocation inside gcp_spec.c (internal RAM by preference — see the buffer
 * note there, it is worth a third of the bit rate), so this is NOT reentrant. */
typedef struct {
    int  fill;    /* samples in the window being filled */
    int  m;       /* windows folded so far              */
    bool ok;      /* buffers present; false = disabled  */
} gcp_spec_t;

void gcp_spec_begin(gcp_spec_t *sp);
void gcp_spec_push(gcp_spec_t *sp, int ones);
/* Normalised spectral entropy H/ln(K) ∈ (0,1] of what was pushed, and the
 * window count it was built from. False when the run was too short, the
 * buffers are missing, or the accumulated power was zero. */
bool gcp_spec_finish(gcp_spec_t *sp, double *out_h, int *out_m);

/* The null moments of H/ln(K) at `m` Welch windows — see gcp_spec.c for the
 * derivation. Always computed from the run's ACTUAL m, never a constant: m
 * follows the segment count, which follows ?run= and the camera's delivered
 * rate. A hard-coded pair would silently mean something else at another window
 * length, the same defect gcp_z_per_bias() exists to prevent for the bias bar. */
/* ── The periodogram itself (2026-08-26) ───────────────────────────────────
 * gcp_spec_finish() reduces the accumulator to ONE number, and H is blind to
 * WHERE the power sits — a line at bin 256 and the same power spread over
 * bins 200..300 give different H, but neither says "bin 256". That location is
 * the whole question when asking whether the structure follows the sensor's
 * ROW geometry: extraction runs in raster order, so a spatial period aliases
 * to a fixed segment period and therefore to a fixed bin, while a timing clock
 * (MIPI/CSI) cannot — the bits are read from a completed frame in PSRAM, not
 * synchronously with the lane.
 *
 * Copies out the accumulated Welch periodogram, NORMALISED to sum 1 over the
 * K published bins so nodes are directly comparable regardless of window count.
 * Valid between gcp_spec_finish() and the next gcp_spec_begin(), and that is
 * ENFORCED: called at any other time it returns false rather than handing back
 * a partial accumulation carrying the previous run's window count. `n` is the
 * caller's array length and must be >= GCP_SPEC_BINS. */
bool gcp_spec_bins(float *out, int n, int *out_m);

bool gcp_spec_null(int m, double *out_h0, double *out_sd);
/* (H_norm − H₀)/σ at `m` windows. NEGATIVE = less spectral disorder than white
 * noise, which is the direction of interest. */
bool gcp_spec_z(double h_norm, int m, double *out_z);

/* The packed real FFT against a reference DFT on this node's own silicon.
 * Returns true when the worst relative bin error is under 1e-3 and writes it to
 * *out_worst. Served by GET /spectest, and the same argument as /camtest:
 * a wrong-but-plausible H looks exactly like a result.
 * ⛔ Do not change spec_fold() without it. */
bool gcp_spec_selftest(double *out_worst);

/* True when the FFT working buffers had to fall back to PSRAM because internal
 * RAM was exhausted. ⚠ Not cosmetic: measured 2026-08-25, a node in that state
 * runs at ~3,8 Mbit/s against ~5,7 for its peers, because the buffers then share
 * the bus the extractor streams frames over. Published by /spectest. */
bool gcp_spec_in_psram(void);
/* Failed FFT-buffer allocations. Non-zero = this node lost the entropy channel
 * at least once; it retries on a backoff and recovers on its own, but the
 * count is what distinguishes "reported no H" from "cannot compute H".
 * gcp_spec_ready() is the live state: false = no buffers right now. */
int  gcp_spec_alloc_fails(void);
bool gcp_spec_ready(void);

/* gcp_zscore_raw() with the spectral accumulator running alongside. The z
 * arithmetic is the SAME loop and stays bit-identical — `sp` only receives the
 * `ones` the z path already counted. Pass NULL for `sp` and this is exactly
 * gcp_zscore_raw(). */
gcp_result_t gcp_zscore_spec(int nseg, bool (*on_yield)(void), double *out,
                             gcp_spec_t *sp);

/* gcp_zscore_spec() with the PRE-FOLD z alongside (2026-08-26).
 *
 * The XOR fold maps a raw bias e to ~2e², so it suppresses a MEAN-BIAS effect
 * by sqrt(2)*e — a factor ~7000 at e = 1e-4. That is the quantity a GCP-style
 * experiment is looking for, so the unfolded stream is scored and archived too.
 * The fold stays: without it there is no stable null (D17).
 *
 * ⚠ RANKING AND ARCHIVE ONLY, never a p-value. Per-node raw sigma measured
 * 1,06..1,28 on certified rungs against 0,997..1,001 folded (2026-08-27), and
 * it leaves that band fast when the light does — 1,69 at mean_px 5, above 10
 * over-lit. Combined and block-centred the channel runs at sigma 2..4. Same
 * compromise the entropy channel makes, for the same reason.
 * ⚠ It covers MORE bits than the folded z over the same window in TIME.
 * ⚠ The folded z is bit-identical to what gcp_zscore_raw() returns.
 * NULL out_pre, or a node without the parallel ring, disables it. */
gcp_result_t gcp_zscore_pre(int nseg, bool (*on_yield)(void), double *out,
                            gcp_spec_t *sp, double *out_pre);

#ifdef __cplusplus
}
#endif
