#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// OV5647 noise source: photon shot + read noise from non-overlapping frame
// pairs. ⚠ "dark frame" survives in names and docs as a LABEL only — the
// enclosure is lit with constant ambient light, and the dark end of the
// exposure ladder is gated off because too few photons stop whitening the LSB.
// This component is SHARED with the slave repo
// via EXTRA_COMPONENT_DIRS — one source of truth, byte-identical extraction on
// both nodes, so a change here means rebuilding and flashing both.

/* ⚠ These cores have no Zbb, so __builtin_popcount is a CALL to __popcountsi2
 * (verified with objdump, and again in gcp.c's disassembly on 2026-08-19). Any
 * hot loop counting bits must use this instead. Public rather than private to
 * elotto_camera because the GCP primitive needs it too: gcp_zscore_raw() ran
 * SEVEN library calls per segment, ~913.000 per run at run=5.
 *
 * Held against __builtin_popcount over a value sweep by GET /camtest
 * (`popcount_ok`) — the extractor's own self-test compares emitted WORDS, so it
 * never exercised this directly, and it now feeds a z. */
static inline uint32_t cam_popcount32(uint32_t v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    return (v * 0x01010101u) >> 24;
}

typedef struct {
    bool     ready;               // stream running, ring buffer filling
    uint64_t frame_pairs;         // non-overlapping diff pairs processed
    uint64_t bits_extracted;      // total LSB-diff bits packed
    uint64_t ones_count;          // running ones count, for bias
    uint32_t stuck_frame_count;   // pairs where both frames were byte-identical
    double   bias;                // ones_count / bits_extracted (ideal 0.5)
    double   sigma;                // stddev of per-mini-run z (ideal 1.0)
    int      sigma_samples;       // number of mini-runs folded into sigma (gate wants >=200)
    double   autocorr_lag[4];     // lag-1..4 word-stream bit autocorrelation (ideal 0)
    double   mean_pixel_level;    // running mean raw pixel byte, light-leak check (black floor)
    double   mbit_per_sec;        // sustained PRODUCTION rate: what extraction wrote
                                  // into the ring, over wall time. ⚠ NOT what a
                                  // measurement consumed -- a node that produces
                                  // faster than it reads throws the surplus away
                                  // (ring_drops), and this number counts the
                                  // discarded words too. Measured 2026-08-30: the
                                  // master reads 5,71 here against 3,34 on the
                                  // slaves purely because its consumer is slower
                                  // and its ring overflows. Use it for the SENSOR
                                  // ceiling (D23) and for /camtest; for device
                                  // performance read consume_mbit_per_sec below.
    /* What MEASUREMENT actually consumed, per second SPENT READING (2026-08-30).
     * Bits handed to a caller of camera_read_word()/_raw(), over the time that
     * reading took -- gaps between runs excluded, because nothing is read then
     * and averaging them in describes the duty cycle, not the device.
     *
     * This is the number that compares nodes: they read the same words in the
     * same window, so a node that falls behind shows up here and nowhere else.
     * 0 until the first run reports one. */
    double   consume_mbit_per_sec;
    double   zero_diff_frac;      // fraction of pixels with diff==0 (noise below 1 ADU).
                                  // Diagnostic: diff==0 has LSB 0, so a high value here
                                  // directly explains a deficit-of-ones bias.
    /* ── PRE-FOLD, i.e. the stream the SENSOR produced (2026-08-26) ────────
     * Everything above describes the stream after the XOR fold. These two
     * describe it before, and they are the front-end health the instrument
     * used to be blind to: on 2026-08-26 a fold-off node read raw_bias 0,4934
     * and raw_sigma 1,7233 where its folded peers were at 0,49994 / 1,0080,
     * and seeing that took a firmware flash. Same definitions as `bias` and
     * `sigma`, same 3200-bit mini-run.
     * ⚠ With the fold OFF these are the SAME bits as `bias`/`sigma` and the
     * pairs must agree; a divergence there is a bug, not a finding. */
    double   raw_bias;            // pre-fold ones / pre-fold bits (ideal 0.5)
    double   raw_sigma;           // stddev of per-mini-run z, pre-fold (ideal 1.0)
    int      raw_sigma_samples;   // mini-runs folded into raw_sigma
    uint64_t raw_bits;            // pre-fold bits == pixels consumed
    /* ── The RUNS channel, pre-fold (2026-08-27) ──────────────────────────
     * The entropy a rung delivers is short by two terms: one from the bits
     * being unbalanced, one from them depending on each other. `raw_bias` is
     * the first term — it IS the monobit test. This is the second.
     *
     * `raw_runs_z` is the NIST runs statistic over the pre-fold stream, with
     * one deliberate departure: E[V] and SD[V] are taken from the OBSERVED
     * proportion of ones, not from 1/2. NIST refuses to run the runs test at
     * all unless |pi - 1/2| <= 2/sqrt(n), and at these bit counts that bar is
     * ~6e-4 against a raw bias that misses 0,5 by up to 5e-3 — every rung
     * would be refused. Conditioning on the observed pi is what makes the
     * number useful HERE: it measures clustering GIVEN the imbalance, so it
     * says something monobit does not, instead of restating it.
     *
     * Sign is informative. NEGATIVE = fewer runs than the marginal implies,
     * i.e. the bits clump — which is the dark-end failure, where zero pixel
     * differences give deterministic zeros and those sit together in dark
     * regions. POSITIVE = more runs than implied, i.e. alternation.
     * ⚠ NOT a gate and NOT in the ranking (D46, D55). Measured and published
     * per sweep rung. The ranking channel that used it was deleted after one
     * session showed it underdispersed and orthogonal to the Pre outliers.
     * ⚠ 0,0 means the channel was not armed or the window is empty — it is not
     * a reading of "perfectly random". Check `raw_trans`. */
    uint64_t raw_trans;           // pre-fold adjacent bit pairs that differ
    double   raw_runs_z;          // NIST runs z, conditioned on the observed bias

    /* P4 die temperature, the covariate for the raw-channel offset monitor
     * (D44). ⚠ It is the SoC die, NOT the OV5647 — a proxy that shares the
     * enclosure, never to be reported as sensor temperature.
     * ⚠ NAN when the driver did not install. Handle it; do not print 0,0. */
    float    die_temp_c;

    uint32_t ring_drops;          // words discarded: consumer behind, ring full
    uint32_t consumer_waits;      // times a read had to wait for the producer (normal
                                  // backpressure -- the GCP task outruns the sensor)
    uint32_t stalls;              // reads that gave up: the node is faulted and
                                  // rebooted, since there is no second source
    /* Wall-time accounting for ONE frame pair, in milliseconds, averaged over
     * the current window. ms_pair is measured boundary to boundary and is the
     * ground truth; the other three are its parts, and what they do not add up
     * to is the capture task's yield plus preemption from above it.
     *
     * ⚠ Read ms_wait first. It is the time the loop sat blocked in DQBUF, i.e.
     * waiting for the sensor. If it is large, the pair rate is set by the frame
     * rate and NOTHING done to the extraction code can raise it -- which is the
     * one reading that explains two CPU savings measuring 0,0 %. */
    double   ms_pair;
    double   ms_wait;
    double   ms_extract;
    double   ms_rest;
} camera_stats_t;

// Bring up MIPI-CSI + OV5647, disable AEC/AGC, apply fixed exposure/gain from
// Kconfig, start capture task. Pin/register values MUST be set via
// `idf.py menuconfig` -> "Elotto Camera (OV5647) Configuration" to match the
// actual CSI wiring before this will do anything useful.
esp_err_t camera_init(void);

bool camera_is_ready(void);
void camera_get_stats(camera_stats_t *out);

/* ── Calibration control (docs/PLAN.md Task 1) ─────────────────────────────
 *
 * Everything in camera_stats_t is cumulative since the stream started (or since
 * the last reset). That is why a reset exists at all: to score a candidate
 * sensor setting you need the statistics of a WINDOW, and without this a
 * measurement taken after changing exposure is still dominated by the setting
 * before it.
 *
 * Order for evaluating one candidate:
 *   camera_set_exposure(e, g)      -> false if the sensor did not latch it
 *   camera_stats_reset(settle)     -> discard `settle` pairs, empty the ring,
 *                                     then zero the statistics
 *   wait for camera_stats_settled()
 *   ...let bits accumulate...
 *   camera_get_stats(&s)           -> describes only this window
 *
 * Must not run while a measurement is consuming words: the reset empties the
 * ring, and a session drawing from it would be reading discarded entropy.
 */

/* Drop every word waiting in the ring and the packer's partial word, then wait
 * for `pairs` freshly extracted frame pairs. Statistics are NOT touched, so the
 * block's camera health survives — that is the whole difference from
 * camera_stats_reset(). Asynchronous: poll camera_ring_flushed().
 *
 * Called before every measurement window so the bits credited to an item were
 * physically captured during it. Without it the ring is FULL at a window's
 * start, having filled through the preceding gap: 524288 bits, which is 10 %
 * of a run=1 item and 2 % of a run=5 one, collected before the item existed. */
void camera_ring_flush(int pairs);
bool camera_ring_flushed(void);

// Discard `settle_pairs` frame pairs (>=1), empty the ring, then zero every
// entropy statistic and restart the rate clock. Asynchronous: the capture task
// performs it, so poll camera_stats_settled().
void camera_stats_reset(int settle_pairs);
bool camera_stats_settled(void);

// Apply and VERIFY BY READ-BACK. Returns false if the sensor did not take the
// value — a silently ignored write would make calibration score the previous
// setting and then "choose" it. exposure 1..0xFFFF, gain 0..0x3FF.
//
// The exposure range is the one the REGISTERS can hold, not the one the OV5647
// datasheet lists: 0x3500[3:0]/0x3501/0x3502[7:4] carry 16 integer bits, so a
// value above 0xFFFF would be truncated on the way in and read back as a
// different number. Clamping to what round-trips keeps set/get honest.
bool camera_set_exposure(uint32_t exposure, uint32_t gain);
void camera_get_exposure(uint32_t *exposure, uint32_t *gain);

bool camera_get_xor_fold(void);
/* Capture geometry. The WIDTH is the row length a spatial period in the bit
 * stream aliases against (was /specdump; endpoint deleted with the spectral channel). */
void camera_get_geometry(uint32_t *w, uint32_t *h);

/* Time `frames` frames with the extraction STOPPED and every buffer queued, and
 * return the frames per second the sensor delivers on its own. Blocks for about
 * frames/fps seconds; the capture task does the work.
 *
 * ⚠ This is the number that says whether the bit rate is walled by the sensor
 * or by us. The live loop's ms_wait cannot: a fast sensor whose frames cannot
 * be written while the CPU hammers PSRAM waits exactly like a slow one. Run
 * with the CPU otherwise idle and compare against the live pair cycle.
 * Restarts the statistics window, since the probe itself extracts nothing. */
double camera_fps_probe(int frames, int timeout_ms);

/* ── The sweep (§1.4) ──────────────────────────────────
 *
 * Objective, settled in §1.7 and NOT a tunable: **best entropy, not maximum
 * rate**. A candidate must pass every hard gate below; among those that do, the
 * fastest wins. Rate is the tie-break and never a reason to accept a measurably
 * less uniform stream — the entropy is the instrument.
 *
 * Runs on master and slave from the same source, so the two cannot drift apart
 * in what "calibrated" means. Each node calibrates its OWN camera and they will
 * land on different settings; the cameras are physically different units and
 * nothing requires them to share an operating point. What they must still share
 * is the segment count per run, which travels on the wire.
 *
 * MUST NOT run while a measurement is consuming words: every step empties the
 * ring, so a session drawing from it would be reading discarded entropy.
 */

// Which gate a candidate failed. 0 = passed all of them. Reported per step so
// a sweep that chose nothing says WHY rather than just "no".
#define CAM_CAL_FAIL_APPLY   0x01   // sensor did not latch the setting (read-back)
#define CAM_CAL_FAIL_BITS    0x02   // too few bits in its slice to score at all
#define CAM_CAL_FAIL_BIAS    0x04   // legacy; folded bias no longer gated (D52)
#define CAM_CAL_FAIL_AUTOC   0x08   // some |autocorr lag 1..4| >= 0.01
#define CAM_CAL_FAIL_SIGMA   0x10   // per-mini-run sigma outside 1 +- 0.05
#define CAM_CAL_FAIL_STUCK   0x20   // a frame pair came back byte-identical
#define CAM_CAL_FAIL_LIGHT   0x40   // legacy; bright mean_px no longer gated (D52)
#define CAM_CAL_FAIL_DARK    0x80   // mean pixel level below the shot-noise floor
#define CAM_CAL_FAIL_ZDIFF  0x100   // too many zero pixel differences
#define CAM_CAL_FAIL_RSIG   0x200   // PRE-FOLD per-mini-run sigma above the bar

#define CAM_CAL_MAX_STEPS   12

// One candidate setting and the window that scored it. The whole table is kept
// and published: the Task 1 gate asks for a bias-vs-exposure CURVE, and a curve
// is the only way to tell a real response from noise around one lucky point.
typedef struct {
    uint32_t exposure, gain;
    bool     xor_fold;
    uint64_t bits;              // bits in this window (0 = never measured)
    int      minirun_n;         // mini-runs behind `sigma`
    double   bias, sigma, mbit_per_sec;
    double   autocorr_max;      // max |lag 1..4|
    double   mean_pixel_level, zero_diff_frac;
    /* PRE-FOLD, for this candidate's window (D43). GATED since 2026-08-27 —
     * both of them, and the comment this replaces was the reason it took so
     * long: a naive |raw_sigma-1| <= 0,05 bar WOULD reject rungs the array uses
     * successfully, so the bar is one-sided and set from the measured gap
     * instead. See CAL_MAX_RAW_BIAS and CAL_RAW_SIGMA_MAX in camera.c. */
    double   raw_bias, raw_sigma;
    double   raw_runs_z;        // pre-fold runs statistic; measured, NOT gated
    uint64_t raw_bits;          // pre-fold bits behind raw_bias/raw_runs_z
    uint32_t stuck_frames;
    uint32_t fail;              // CAM_CAL_FAIL_* bitmask, 0 = passed
} camera_cal_step_t;

typedef struct {
    bool     ok;                // a candidate passed every gate and was applied
    int      chosen;            // index into step[], -1 if none passed
    uint32_t exposure, gain;    // what the camera is running on now
    bool     xor_fold;
    double   bias, sigma, mbit_per_sec, autocorr_max, mean_pixel_level;
    double   raw_bias, raw_sigma;   // PRE-FOLD, of the setting actually chosen (D43)
    double   raw_runs_z;            // PRE-FOLD runs statistic of that setting
    bool     kept;                  // the incumbent rung was kept (hysteresis)
    int      nsteps;
    uint32_t elapsed_ms;
    /* The z-per-unit-bias the bias gate was scaled to (see
     * camera_cal_set_z_scale). 0 = the sweep ran on the legacy constant. A
     * published sweep cannot be read without it: the same bias is a different
     * verdict at a different run length. */
    double   z_scale;
    camera_cal_step_t step[CAM_CAL_MAX_STEPS];
} camera_cal_t;

/* Tell the sweep what a unit of bias is WORTH in this session, so its bias gate
 * can mean the same thing at every run length.
 *
 * `z_per_bias` is the per-run z offset produced by a bias of 1.0, i.e.
 * GCP_SEGMENT_BITS * sqrt(segments) / GCP_SEGMENT_SD for the segment count the
 * session will run. The constant lives at the call sites rather than here
 * because elotto_gcp already REQUIRES elotto_camera and the dependency must not
 * become circular -- and because GCP_SEGMENT_SD is the one literal in this
 * project that must never be restated (see components/elotto_gcp/gcp.h).
 *
 * 0 restores the legacy fixed bar. Set it before camera_calibrate(); it is
 * remembered until changed. */
void camera_cal_set_z_scale(double z_per_bias);

/* Sweep the exposure ladder, score each candidate over its own window, apply the
 * best setting that passes every gate, and verify it by read-back.
 *
 * `out` is ~1.2 KB — allocate it in PSRAM once, not on a task stack.
 *
 * If NO candidate passes, the setting in force at entry is restored and `ok` is
 * false. That is deliberate: "best of a bad lot" would move the operating point
 * to something no gate certified, which is the one way this could quietly change
 * the measured physics. The caller keeps measuring on the known setting instead.
 *
 * `abort_cb` (may be NULL) is polled while waiting; returning true ends the sweep
 * early, restores the entry setting and returns false. The slave passes one that
 * also pumps its socket, since its command loop is blocked for the whole sweep.
 *
 * Returns true if a gated setting was applied.
 */
bool camera_calibrate(int budget_ms, bool (*abort_cb)(void), camera_cal_t *out);

/* Serialise a sweep as JSON onto an open HTTP request, chunk by chunk (the full
 * table is ~1.2 KB and does not belong on a handler's stack).
 *
 * Shared because BOTH firmwares serve it: the master at GET /calibrate, and each
 * slave at the same path. A per-node optical fault — one sensor dispersing more
 * than its neighbours — cannot be diagnosed from the master's own ladder, and
 * the wire protocol deliberately carries only the CHOSEN rung, not the sweep.
 * Without this the slaves' sweeps were computed, stored in PSRAM, and thrown
 * away unread.
 *
 * Pass c = NULL (or a sweep that never ran) to emit {"ran":false,"steps":[]}. */
esp_err_t camera_cal_send_json(void *httpd_req, const camera_cal_t *c);

/* Handle `POST /expose?exp=<lines>[&gain=<g>]` — set this node's operating point
 * by hand, for tuning the physical light against a live mean_px reading.
 *
 * Shared for the same reason as the sweep serialiser: both firmwares serve this
 * path (the master for its own camera, each slave for its own), and the clamps
 * and the read-back semantics must be identical or /diag would show four nodes
 * answering the same request differently.
 *
 * `busy` is the caller's "a measurement is in flight" predicate — the master
 * passes its session state, the slave passes g_measuring. Refused with 409 when
 * busy: writing a sensor register mid-run changes the instrument underneath a
 * z-score that is already being accumulated, and the run would be labelled with
 * a setting it was only half measured at.
 *
 * Omitting `gain` keeps the gain in force, so an exposure change cannot silently
 * move the other parameter. Replies with the READ-BACK setting, not the
 * requested one — camera_set_exposure() verifies by re-reading the registers,
 * and a request the sensor refused must not be reported as applied. */
esp_err_t camera_expose_handle(void *httpd_req, bool busy);

/* GET handler for the extraction self-test + micro-benchmark, served by every
 * node for its own silicon. Answers 409 while a session is measuring: it burns
 * CPU on the same core as the extraction task and allocates ~600 KB of PSRAM.
 * See extract.h for what it actually proves. */
esp_err_t camera_selftest_handle(void *httpd_req, bool busy);

// Priority of the extraction task created by camera_init().
#define ELOTTO_CAM_TASK_PRIO 4

// Core the extraction task is PINNED to. The consumer belongs on the OTHER one,
// which is what ELOTTO_CAM_CONSUMER_CORE names, so both firmwares derive the
// split from one number instead of each spelling out a core id.
//
// IMPORTANT — this is not tuning, it is a fix for a measured factor of two.
// Both tasks were created with xTaskCreate(), i.e. tskNO_AFFINITY, and the P4
// has two cores. The master creates elotto_task fresh on EVERY /start, so the
// scheduler placed it anew each session: on the free core the two had one core
// each, on the camera's core they shared one and BOTH halved. Measured
// 2026-08-30 on the master, same firmware, same parameters:
//
//   sharing a core:  ms_extract 80,0  ms_wait 17,4  prod 3,20  cons 2,88
//   one core each:   ms_extract 46,9  ms_wait  6,9  prod 5,71  cons 5,77
//
// i.e. focus_win_ms 10210 against 5134 — twice the measuring time per item,
// bought nothing. Consumption halving EXACTLY (5,77 -> 2,88) is the signature.
// It hit 4 of 11 session starts and was invisible at idle (10 of 10 idle boots
// fast), because with no session there is no second task to collide with. Two
// sessions inside ONE boot came out in different modes, and that is what ruled
// out every boot-time explanation — PSRAM speed, cache, buffer placement.
// ms_wait rises along with it because CAM_BUF_COUNT is 4 and the loop holds
// two: a stretched extraction outlasts the two free buffers and capture stalls.
// See D61.
#define ELOTTO_CAM_TASK_CORE     1
#define ELOTTO_CAM_CONSUMER_CORE 0

// IMPORTANT — task priority: the extraction task is CPU-hungry (~7.6M pixel
// ops/s). The task calling camera_read_word() MUST run ABOVE
// ELOTTO_CAM_TASK_PRIO, or the producer starves the consumer and measurement
// slows by an order of magnitude while the ring sits permanently full
// (symptom: drops huge, waits == 0, runs 10x too long).
// The master's elotto_task is created at priority 5; the slave's UDP command
// loop runs in its own "link" task created at ELOTTO_CAM_TASK_PRIO + 1. Neither
// may live in app_main, whose priority IDF hardcodes to 1 — below this one.
//
// The priority relation above and the core split are SEPARATE requirements and
// both must hold. Priority decides who wins once they are on one core; the
// pinning is what keeps them off one core at all. Creating either task with
// xTaskCreate() instead of xTaskCreatePinnedToCore() re-opens the factor of two
// documented at ELOTTO_CAM_TASK_CORE, and it returns intermittently — roughly
// one session in three — so a single fast run proves nothing.
//
// Phase 1 consumer API: pop one 32-bit word of extracted entropy.
// Blocks (vTaskDelay) while the ring is empty -- bits are never reused or
// fabricated to cover an underrun. Returns false only if the camera is not
// streaming or has produced nothing for CAM_STALL_TIMEOUT_MS.
//
// There is no fallback source to hand off to: false means this node has stopped
// being an instrument, and the caller must fault it (report + reboot) rather
// than substitute bits from anywhere else.
bool camera_read_word(uint32_t *out);
/* Report one completed read span: how many BITS were handed out and how long the
 * reading took. Called ONCE per run by the consumer, not per word -- a timestamp
 * per word would cost more than the read. Feeds consume_mbit_per_sec; cleared by
 * the same stats reset as the rest of the window. */
void camera_note_consumed(uint64_t bits, int64_t us);
/* The same word, plus the PRE-FOLD ones count of the pixels it came from, taken
 * as one unit so a folded and an unfolded z cover the SAME window (2026-08-26).
 * With the fold on a word is 64 pixels (0..64 ones), with it off 32.
 * ⚠ *out_raw is 0 when this node has no parallel ring — check
 * camera_raw_stream_ok() rather than reading 0 as a count.
 * ⚠ Never interleave with camera_read_word() inside one window: both advance
 * the same tail, and the raw total would silently cover fewer bits. */
bool camera_read_word_raw(uint32_t *out, uint32_t *out_raw);
bool camera_raw_stream_ok(void);
