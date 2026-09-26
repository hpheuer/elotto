/* ── The GCP measurement primitive, shared by master and slave ──────────
 *
 * The one function that turns camera bits into a z-score, compiled into BOTH
 * firmwares from this single definition. The combine is Sum(z_node)/sqrt(k)
 * over nodes that measured the same window, which is only meaningful if every
 * node computed z the same way — one definition, so the nodes cannot disagree;
 * a wrong-but-plausible z looks exactly like a result.
 *
 * The call sites differ only in abort handling, which is the `on_yield`
 * parameter rather than a forked copy of the loop.
 */
#pragma once

#include <stdbool.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One segment is seven 32-bit words, all bits used. LSB bits as measured: each
 * word is 32 raw LSBs. For a fair coin that is mean 112 and sd sqrt(224*0.25)
 * = sqrt(56). */
#define GCP_SEGMENT_BITS   224
#define GCP_SEGMENT_MEAN   112.0
#define GCP_SEGMENT_SD     7.4833147735

/* The same mean as an int, for the one place the subtraction can be done in
 * integers. ones is in [0,224], so ones - 112 converts to double exactly. */
#define GCP_SEGMENT_MEAN_I 112

/* What a unit of stream bias is WORTH as a per-run z offset, at this run length.
 *
 * A bias b over `segments` segments moves the ones count by (b - 0,5)*224*nseg
 * and the z by that over GCP_SEGMENT_SD*sqrt(nseg), i.e. the offset grows with
 * sqrt(nseg). Every threshold on a bias therefore has to be converted here rather
 * than written down as a constant, or it silently means something different at
 * a different ?run=. Shared for the same reason as the rest of this header:
 * master and slave must apply the same bar to their own cameras.
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

/* One run of `nseg` segments, consuming `nseg * 224` bits from the camera.
 *
 * `on_yield` is called after each of the ~4 yields per run and returns false to
 * abandon the run (the slave polls its abort socket there). Pass NULL when
 * there is nothing to poll — the master aborts between runs, not inside one.
 * The yield cadence is a fixed fraction of the run rather than a fixed count,
 * so it stays matched across nodes at every run length: per-run wall time is
 * the max over nodes, so a mismatch would slow every measurement to the
 * slowest device.
 *
 * `*out` is the binomial z of the whole window (the stream is LSB).
 * Half-window: *out_h1 / *out_h2 are the same bits split at the frame-
 * pair boundary nearest nseg/2, nseg/2 when the window holds none
 * (NULL disables). nseg < 2 leaves both at 0. Written only on GCP_OK. */
gcp_result_t gcp_zscore_raw(int nseg, bool (*on_yield)(void), double *out);

gcp_result_t gcp_zscore_pre(int nseg, bool (*on_yield)(void), double *out,
                            double *out_h1, double *out_h2);

#ifdef __cplusplus
}
#endif
