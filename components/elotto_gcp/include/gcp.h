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

#ifdef __cplusplus
}
#endif
