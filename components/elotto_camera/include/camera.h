#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// OV5647 dark-frame noise source (docs/PLAN_4NODE.md): photon shot + read noise
// from non-overlapping frame pairs. This component is SHARED with the slave repo
// via EXTRA_COMPONENT_DIRS — one source of truth, byte-identical extraction on
// both nodes, so a change here means rebuilding and flashing both.

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
    double   mbit_per_sec;        // sustained extraction rate
    double   zero_diff_frac;      // fraction of pixels with diff==0 (noise below 1 ADU).
                                  // Diagnostic: diff==0 has LSB 0, so a high value here
                                  // directly explains a deficit-of-ones bias.
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

/* ── The sweep (docs/PLAN.md Task 1 §1.4) ──────────────────────────────────
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
#define CAM_CAL_FAIL_BIAS    0x04   // |bias - 0.5| >= 1e-3
#define CAM_CAL_FAIL_AUTOC   0x08   // some |autocorr lag 1..4| >= 0.01
#define CAM_CAL_FAIL_SIGMA   0x10   // per-mini-run sigma outside 1 +- 0.05
#define CAM_CAL_FAIL_STUCK   0x20   // a frame pair came back byte-identical
#define CAM_CAL_FAIL_LIGHT   0x40   // mean pixel level above the light-leak floor

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
    uint32_t stuck_frames;
    uint32_t fail;              // CAM_CAL_FAIL_* bitmask, 0 = passed
} camera_cal_step_t;

typedef struct {
    bool     ok;                // a candidate passed every gate and was applied
    int      chosen;            // index into step[], -1 if none passed
    uint32_t exposure, gain;    // what the camera is running on now
    bool     xor_fold;
    double   bias, sigma, mbit_per_sec, autocorr_max, mean_pixel_level;
    int      nsteps;
    uint32_t elapsed_ms;
    camera_cal_step_t step[CAM_CAL_MAX_STEPS];
} camera_cal_t;

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

// IMPORTANT — task priority: the extraction task is CPU-hungry (~7.6M pixel
// ops/s). The task calling camera_read_word() MUST run ABOVE
// ELOTTO_CAM_TASK_PRIO, or the producer starves the consumer and measurement
// slows by an order of magnitude while the ring sits permanently full
// (symptom: drops huge, waits == 0, runs 10x too long).
// The master's elotto_task is created at priority 5; the slave's UDP command
// loop runs in its own "link" task created at ELOTTO_CAM_TASK_PRIO + 1. Neither
// may live in app_main, whose priority IDF hardcodes to 1 — below this one.
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
