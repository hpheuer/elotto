#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "linux/videodev2.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_http_server.h"
#include "camera.h"
#include "extract.h"

// OV5647 noise source. See include/camera.h on why "dark frame" is a label.
// Extraction: non-overlapping frame pairs, diff = f[2k+1] - f[2k] per pixel
// (cancels fixed-pattern noise exactly), LSB of each diff packed into a
// ring buffer, XOR-folded. camera_read_word() feeds noise_word() in sensor.c.
// SHARED between the master and slave repos -- a change here affects both nodes.

static const char *TAG_CAM = "cam";

/* 4, and raising it does nothing: measured at 8, the pair cycle stayed at
 * 56,0 ms and only the split moved (wait 15,1 -> 11,9 ms, extraction 39,1 ->
 * 42,2 ms). The loop is not short of buffers, it is short of frames -- see the
 * frame-rate probe below. */
#define CAM_BUF_COUNT       4
#define RING_WORDS          16384          // 64 KB of uint32_t words
#define WORDS_PER_MINIRUN   100            // 3200 bits/mini-run, Var(popcount)=800
#define MINIRUN_BITS        (WORDS_PER_MINIRUN * 32)

static int      s_fd = -1;
static uint32_t s_frame_size = 0;
static uint8_t *s_bufs[CAM_BUF_COUNT];
#if CONFIG_ELOTTO_CAM_XCLK_PIN > 0
static esp_cam_sensor_xclk_handle_t s_xclk_handle;
#endif

static SemaphoreHandle_t s_mutex;
static camera_stats_t    s_stats = { 0 };

// Producer-only running state (camera_task is the sole writer; publish_stats()
// copies a snapshot into s_stats under s_mutex for readers).
// s_ring lives in PSRAM: 64 KB of static internal DRAM would not fit alongside
// the existing sensor.c tables (g_status.results[], s_zsum[]) on this P4.
static uint32_t *s_ring;
// Single-producer (camera_task) / single-consumer (GCP task) ring. head is
// written only by the producer, tail only by the consumer, so no lock is
// needed -- but both must be volatile so neither side caches the other's index.
static volatile uint32_t s_ring_head = 0;
static volatile uint32_t s_ring_tail = 0;
static volatile uint32_t s_ring_drops = 0;
static volatile uint32_t s_consumer_waits = 0;
static volatile uint32_t s_stalls = 0;

#define CAM_STALL_TIMEOUT_MS  2000
/* Frame pairs between idle-task yields; see camera_task(). */
#define CAM_YIELD_PAIRS       4
static uint64_t s_bits_extracted = 0, s_ones_count = 0;
static uint64_t s_run_ones = 0, s_run_bits = 0;
static double   s_z_mean = 0.0, s_z_m2 = 0.0;
static int      s_z_n = 0;
// Autocorrelation: count positions where bit i AND bit i+L are both 1, so the
// estimator can be mean-centred in publish_stats(). An agreement-rate estimator
// would fold the stream's own bias into r (bias b inflates r by ~4b^2/(1-4b^2))
// and report correlation that isn't there.
static uint64_t s_autocorr_both1[4] = { 0 };
static uint64_t s_autocorr_pairs[4] = { 0 };
static uint64_t s_pixel_sum = 0, s_pixel_n = 0;
static uint64_t s_zero_diffs = 0, s_diff_n = 0;
static int64_t  s_stream_start_us = 0;

/* ── Where a frame pair's wall time actually goes ──────────────────────────
 * Producer-only, published per pair by publish_stats().
 *
 * ⚠ This exists because the extraction cost and the pair cycle stopped adding
 * up: /camtest priced extraction+statistics at ~40 ms per pair while the live
 * cycle was 55,9 ms, and two changes that removed real CPU work from the loop
 * moved the rate by 0,0 %. Either answer is worth having and neither can be
 * inferred from a microbenchmark -- a loop waiting on frames absorbs any CPU
 * saving without changing its rate, and looks exactly like a loop that is
 * compute-bound at a cost the benchmark underestimates.
 *
 * s_us_cycle is measured pair-completion to pair-completion, so it is the
 * ground truth the three components have to add up to; whatever is left over is
 * the yield and preemption by higher-priority tasks. */
static uint64_t s_us_wait = 0;      // blocked in DQBUF, i.e. waiting for frames
static uint64_t s_us_ext = 0;       // inside diff_and_extract()
static uint64_t s_us_rest = 0;      // publish_stats() + the two QBUFs
static uint64_t s_us_cycle = 0;     // whole pair, boundary to boundary
static uint64_t s_acct_pairs = 0;
static int64_t  s_last_pair_us = 0;

/* ── Frame-rate probe ──────────────────────────────────────────────────────
 * The accounting above says how the pair cycle is SPENT; it cannot say what
 * the cycle would be if the CPU were free. Waiting in DQBUF is produced both
 * by a sensor that is genuinely slower than assumed AND by a sensor that is
 * fast but cannot get its frames written while the CPU is reading PSRAM. Those
 * two have opposite consequences -- one is a wall, the other is headroom -- and
 * no amount of staring at ms_wait separates them.
 *
 * So measure the cadence with the extraction stopped: every buffer queued,
 * dequeue and requeue, time nothing else. That is the sensor's own rate. The
 * capture task performs it (it owns the fd), triggered by a flag. */
/* ── Ring flush: drop pending words, then wait for fresh frames ────────────
 * Separate from camera_stats_reset() ON PURPOSE, and the separation is the
 * point. Both empty the ring, but the reset ALSO zeroes bias / sigma /
 * autocorr / mean_pixel and restarts the rate clock — which is right before a
 * calibration candidate and wrong before a measurement item, because those
 * accumulators are what /loops publishes as the block's camera health.
 *
 * Until 2026-08-19 the per-item settle called camera_stats_reset(1), so in an
 * ATTENDED session every block's cam_bias / cam_mbit described only its last
 * item. That is the number this rig uses to decide whether an arm is healthy.
 * A flush must not cost it. */
static volatile int    s_flush_pairs = 0;   // fresh pairs still owed
static volatile bool   s_flush_dropped = false;
static volatile bool   s_flush_done = true;

static volatile int    s_probe_req = 0;      // frames to time, 0 = idle
static volatile bool   s_probe_done = false;
static double          s_probe_fps = 0.0;

static esp_err_t cam_reg_write(uint32_t regaddr, uint32_t value)
{
    esp_cam_sensor_reg_val_t regval = { .regaddr = regaddr, .value = value };
    struct v4l2_ext_control ctrl = {
        .id   = ESP_CAM_SENSOR_IOC_S_REG,
        .p_u8 = (uint8_t *)&regval,
        .size = sizeof(regval),
    };
    struct v4l2_ext_controls ctrls = {
        .ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL,
        .count      = 1,
        .controls   = &ctrl,
    };
    if (ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
        ESP_LOGW(TAG_CAM, "reg write 0x%04x=0x%02x failed", (unsigned)regaddr, (unsigned)value);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t cam_reg_read(uint32_t regaddr, uint32_t *value)
{
    esp_cam_sensor_reg_val_t regval = { .regaddr = regaddr, .value = 0 };
    struct v4l2_ext_control ctrl = {
        .id   = ESP_CAM_SENSOR_IOC_G_REG,
        .p_u8 = (uint8_t *)&regval,
        .size = sizeof(regval),
    };
    struct v4l2_ext_controls ctrls = {
        .ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL,
        .count      = 1,
        .controls   = &ctrl,
    };
    if (ioctl(s_fd, VIDIOC_G_EXT_CTRLS, &ctrls) != 0) return ESP_FAIL;
    *value = regval.value;
    return ESP_OK;
}

// Read back what the sensor actually holds. Writing these registers is not
// proof they stuck: the driver rewrites the whole format array on S_FMT, and
// some OV5647 revisions latch exposure only via group-hold. Without this, a
// silently ignored exposure/gain setting looks identical to a working one.
static void cam_verify_regs(const char *when)
{
    uint32_t aec = 0, e0 = 0, e1 = 0, e2 = 0, g0 = 0, g1 = 0;
    cam_reg_read(0x3503, &aec);
    cam_reg_read(0x3500, &e0); cam_reg_read(0x3501, &e1); cam_reg_read(0x3502, &e2);
    cam_reg_read(0x350a, &g0); cam_reg_read(0x350b, &g1);
    uint32_t exposure = ((e0 & 0x0F) << 12) | ((e1 & 0xFF) << 4) | ((e2 & 0xF0) >> 4);
    uint32_t gain     = ((g0 & 0x03) << 8) | (g1 & 0xFF);
    ESP_LOGI(TAG_CAM, "regs[%s] 0x3503=0x%02x (AEC/AGC manual=%s) exposure=%lu (want %d) "
             "gain=%lu (want %d)",
             when, (unsigned)aec, ((aec & 0x03) == 0x03) ? "yes" : "NO",
             (unsigned long)exposure, CONFIG_ELOTTO_CAM_REG_EXPOSURE,
             (unsigned long)gain, CONFIG_ELOTTO_CAM_REG_GAIN);
}

// Pack one LSB-diff word: update ring buffer, bias, per-mini-run sigma and
// lag-1..4 bit autocorrelation across the word stream (see PLAN Phase 0 gate).
static void process_word(uint32_t w)
{
    // Publish to the consumer first. Never overwrite an unread slot: dropping
    // fresh bits when the consumer is behind is fine (excess entropy), reusing
    // or clobbering them is not.
    uint32_t next = (s_ring_head + 1) % RING_WORDS;
    if (next == s_ring_tail) {
        s_ring_drops++;
    } else {
        s_ring[s_ring_head] = w;
        s_ring_head = next;
    }

    // Statistics cover every extracted word, including dropped ones: /diag must
    // characterise the source itself, not whichever subset got consumed.
    int ones = (int)cam_popcount32(w);
    s_bits_extracted += 32;
    s_ones_count += ones;

    s_run_ones += ones;
    s_run_bits += 32;
    if (s_run_bits >= MINIRUN_BITS) {
        double z = (s_run_ones - MINIRUN_BITS / 2.0) / sqrt(MINIRUN_BITS * 0.25);
        s_z_n++;
        double delta = z - s_z_mean;
        s_z_mean += delta / s_z_n;
        s_z_m2 += delta * (z - s_z_mean);
        s_run_ones = 0;
        s_run_bits = 0;
    }

    // Bits are packed MSB-first, so a bit L positions later in the stream sits
    // L positions lower in the word; (w << L) aligns it. Pairs spanning a word
    // boundary are skipped -- that drops 4 of every 32 pairs but keeps the
    // estimator unbiased and the hot loop cheap.
    for (int L = 1; L <= 4; L++) {
        s_autocorr_both1[L - 1] += (uint64_t)cam_popcount32(w & (w << L));
        s_autocorr_pairs[L - 1] += (uint64_t)(32 - L);
    }
}

/* accumulate_pixel_level() is GONE. It walked the first frame of every pair
 * with a stride of 16 to sample 40000 pixels for mean_pixel_level — but a
 * stride inside 64-byte cache lines still pulls every line, so those samples
 * cost a full 625 KB of PSRAM traffic (~7 ms per pair, ~13 % of the budget) on
 * top of the two frames the diff already reads. The sum now rides along inside
 * the extractor, out of words it has in registers anyway (extract.h). */

// diff = b - a per pixel (mod 256), LSB packed. Non-overlapping: caller never
// reuses a or b as the other side of the next pair.
/* Packer state, owned by the extractor (components/elotto_camera/extract.h).
 * It persists across frame pairs because a pair boundary can land mid-word. */
static cam_pack_t s_pack;
static volatile bool s_xor_fold =
#if CONFIG_ELOTTO_CAM_XOR_FOLD
    true;
#else
    false;
#endif

/* Frame pairs the capture task must throw away before a measurement window
 * opens. After an exposure change the driver still holds frames captured under
 * the OLD setting, and the ring still holds words extracted from them, so a
 * window opened immediately would score a blend of two settings. Set by
 * camera_stats_reset(); the capture task counts it down and zeroes the
 * statistics when it reaches 0, so the window starts clean. */
static volatile int s_settle_pairs = 0;

static void emit_word_cb(uint32_t w, void *ctx) { (void)ctx; process_word(w); }

/* One frame pair. The extraction itself lives in extract.c as two
 * implementations the on-target self-test holds against each other; this picks
 * one and does the frame-level bookkeeping around it.
 *
 * ⚠ `s_xor_fold` is read ONCE here instead of once per pixel. It is volatile,
 * so the per-pixel read could never be hoisted by the compiler, and it can only
 * change between windows anyway (calibration, which also clears the half-pair
 * state through camera_stats_reset). Reading it per frame additionally makes a
 * mid-frame change impossible, which is the behaviour that was always intended. */
static void diff_and_extract(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint32_t zeros = 0, any = 0, psum = 0;
    /* The WORD-WISE path. cam_extract_ref() is still compiled in and is still
     * the definition of correct: GET /camtest runs both over six cases on this
     * silicon and compares the emitted words, the zero count, the stuck verdict
     * and the leftover packer state. Do not switch this line without it. */
    cam_extract_fast(a, b, n, s_xor_fold, &s_pack, emit_word_cb, NULL, &zeros, &any, &psum);

    s_zero_diffs += zeros;
    s_diff_n += n;
    s_pixel_sum += psum;
    s_pixel_n   += n;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_stats.frame_pairs++;
    if (!any) s_stats.stuck_frame_count++;
    xSemaphoreGive(s_mutex);
}

static void publish_stats(void)
{
    double bias  = s_bits_extracted ? (double)s_ones_count / (double)s_bits_extracted : 0.0;
    double sigma = (s_z_n > 1) ? sqrt(s_z_m2 / (s_z_n - 1)) : 0.0;
    double elapsed_s = (esp_timer_get_time() - s_stream_start_us) / 1e6;
    double mbps = (elapsed_s > 0) ? (s_bits_extracted / 1e6) / elapsed_s : 0.0;
    double mean_px = s_pixel_n ? (double)s_pixel_sum / (double)s_pixel_n : 0.0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_stats.ready             = true;
    s_stats.bits_extracted    = s_bits_extracted;
    s_stats.ones_count        = s_ones_count;
    s_stats.bias              = bias;
    s_stats.sigma             = sigma;
    s_stats.sigma_samples     = s_z_n;
    s_stats.mean_pixel_level  = mean_px;
    s_stats.mbit_per_sec      = mbps;
    s_stats.zero_diff_frac    = s_diff_n ? (double)s_zero_diffs / (double)s_diff_n : 0.0;
    s_stats.ring_drops        = s_ring_drops;
    s_stats.consumer_waits    = s_consumer_waits;
    s_stats.stalls            = s_stalls;
    double np = (double)(s_acct_pairs ? s_acct_pairs : 1);
    s_stats.ms_pair           = s_us_cycle / np / 1000.0;
    s_stats.ms_wait           = s_us_wait  / np / 1000.0;
    s_stats.ms_extract        = s_us_ext   / np / 1000.0;
    s_stats.ms_rest           = s_us_rest  / np / 1000.0;
    // Pearson r for two Bernoulli(p) variables: (E[xy] - p^2) / (p(1-p))
    double var = bias * (1.0 - bias);
    for (int L = 0; L < 4; L++) {
        s_stats.autocorr_lag[L] = (s_autocorr_pairs[L] && var > 0.0)
            ? (((double)s_autocorr_both1[L] / (double)s_autocorr_pairs[L]) - bias * bias) / var
            : 0.0;
    }
    xSemaphoreGive(s_mutex);
}

/* Zero every accumulator that describes the entropy and restart the rate clock,
 * so camera_get_stats() from here on describes ONLY the window that starts now.
 *
 * This is the prerequisite for calibration, not a convenience. Every statistic
 * in this file was cumulative since stream start: bias, sigma, autocorrelation,
 * mean pixel level, zero-diff fraction, and mbit_per_sec (which divided by total
 * stream time). Measure setting A, switch to B, read again, and the second
 * number is still mostly A — so no candidate setting could be scored at all.
 *
 * The ring is emptied too: it holds words extracted under the previous setting,
 * and a consumer draining them would be measuring the old configuration.
 *
 * Called from the capture task once the settle countdown expires, so it cannot
 * race the producer. The health counters (ring_drops / consumer_waits / stalls)
 * are deliberately NOT reset — other code reads them as lifetime totals. */
static void stats_reset_locked(void)
{
    s_bits_extracted = 0; s_ones_count = 0;
    s_run_ones = 0; s_run_bits = 0;
    s_z_mean = 0.0; s_z_m2 = 0.0; s_z_n = 0;
    for (int L = 0; L < 4; L++) { s_autocorr_both1[L] = 0; s_autocorr_pairs[L] = 0; }
    s_pixel_sum = 0; s_pixel_n = 0;
    s_zero_diffs = 0; s_diff_n = 0;
    s_us_wait = 0; s_us_ext = 0; s_us_rest = 0; s_us_cycle = 0;
    s_acct_pairs = 0; s_last_pair_us = 0;
    memset(&s_pack, 0, sizeof(s_pack));  // a half-folded pair must not straddle
    s_ring_tail = s_ring_head;           // drop unread old-setting words
    s_stream_start_us = esp_timer_get_time();

    /* The PUBLISHED snapshot has to be zeroed too, not just the accumulators it
     * is computed from. publish_stats() only refreshes it after a pair has been
     * extracted, so a reader polling in the gap between the reset and the next
     * pair would be served the pre-reset window — and would have no way to tell.
     *
     * That is not theoretical: it made the first calibration sweep score every
     * one of its ten candidates on the same stale cumulative snapshot. Each
     * window passed its bit target the instant it opened, so all ten rows came
     * back byte-identical and the sweep "chose" in 2.4 s of a 30 s budget.
     * Zeroing here makes an unmeasured window look empty, which is what it is.
     *
     * The health counters stay cumulative on purpose: ring_drops, consumer_waits
     * and stalls are read elsewhere as lifetime totals. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_stats.frame_pairs       = 0;
    s_stats.stuck_frame_count = 0;
    s_stats.bits_extracted    = 0;
    s_stats.ones_count        = 0;
    s_stats.bias              = 0.0;
    s_stats.sigma             = 0.0;
    s_stats.sigma_samples     = 0;
    s_stats.mean_pixel_level  = 0.0;
    s_stats.mbit_per_sec      = 0.0;
    s_stats.zero_diff_frac    = 0.0;
    s_stats.ms_pair = 0.0; s_stats.ms_wait = 0.0;
    s_stats.ms_extract = 0.0; s_stats.ms_rest = 0.0;
    for (int L = 0; L < 4; L++) s_stats.autocorr_lag[L] = 0.0;
    xSemaphoreGive(s_mutex);
}

static void camera_task(void *arg)
{
    // Hold both frames of a pair dequeued at once rather than copying the
    // first aside: a 640 KB PSRAM-to-PSRAM memcpy per pair cost more time
    // than the diff itself. BUFFER_COUNT=4 leaves 2 queued to the driver
    // while we hold 2.
    struct v4l2_buffer first;
    bool have_first = false;
    int  yield_ctr = 0;
    int64_t pair_wait_us = 0;       // DQBUF time for BOTH frames of this pair
    s_stream_start_us = esp_timer_get_time();

    while (1) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (s_probe_req > 0) {
            /* Give the driver every buffer first: the point is to measure the
             * sensor, so it must never be waiting on us for somewhere to put a
             * frame. The first frame only starts the clock -- it may have been
             * finished long before the probe began. */
            if (have_first) { ioctl(s_fd, VIDIOC_QBUF, &first); have_first = false; }
            int n = s_probe_req, got = 0;
            int64_t tp0 = 0;
            while (got <= n) {
                struct v4l2_buffer pb = {
                    .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    .memory = V4L2_MEMORY_MMAP,
                };
                if (ioctl(s_fd, VIDIOC_DQBUF, &pb) != 0) break;
                if (pb.flags & V4L2_BUF_FLAG_DONE) {
                    if (got == 0) tp0 = esp_timer_get_time();
                    got++;
                }
                ioctl(s_fd, VIDIOC_QBUF, &pb);
            }
            int64_t dtp = esp_timer_get_time() - tp0;
            s_probe_fps = (got > 1 && dtp > 0) ? (double)(got - 1) * 1e6 / (double)dtp : 0.0;
            s_probe_req = 0;
            /* The probe extracted nothing for ~n frame times, which would sit
             * in mbit_s and in the pair accounting as a hole. Start the window
             * again rather than publish a diluted one. */
            stats_reset_locked();
            pair_wait_us = 0;
            s_probe_done = true;
            continue;
        }

        int64_t t_dq = esp_timer_get_time();
        int dq_rc = ioctl(s_fd, VIDIOC_DQBUF, &buf);
        pair_wait_us += esp_timer_get_time() - t_dq;
        if (dq_rc != 0) {
            ESP_LOGW(TAG_CAM, "DQBUF failed");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
            ioctl(s_fd, VIDIOC_QBUF, &buf);
            continue;
        }

        if (!have_first) {
            first = buf;
            have_first = true;
            continue;               // keep this buffer; do not requeue yet
        }

        // Settling after a parameter change: throw the pair away rather than
        // extract from it, then open the window cleanly on the last one.
        if (s_settle_pairs > 0) {
            // Reset FIRST, clear the flag second. camera_stats_settled() is the
            // caller's signal that the window is open, and the caller runs at a
            // higher priority than this task — so a flag cleared before the
            // reset lets it read the previous setting's numbers and call them
            // the new window's.
            if (s_settle_pairs == 1) stats_reset_locked();
            s_settle_pairs--;
            ioctl(s_fd, VIDIOC_QBUF, &first);
            ioctl(s_fd, VIDIOC_QBUF, &buf);
            have_first = false;
            pair_wait_us = 0;       // a discarded pair times nothing
            s_last_pair_us = 0;
            vTaskDelay(1);
            continue;
        }

        /* Drop everything produced BEFORE this pair, then let this pair and any
         * further owed pairs refill the ring. The packer goes too: it can hold
         * up to 31 bits of the previous pair, which would otherwise be the one
         * pre-window remnant left in an otherwise fresh window. */
        if (s_flush_pairs > 0 && !s_flush_dropped) {
            s_ring_tail = s_ring_head;
            memset(&s_pack, 0, sizeof(s_pack));
            s_flush_dropped = true;
        }

        int64_t t_ext0 = esp_timer_get_time();
        diff_and_extract(s_bufs[first.index], s_bufs[buf.index], s_frame_size);
        int64_t t_ext1 = esp_timer_get_time();

        publish_stats();

        ioctl(s_fd, VIDIOC_QBUF, &first);
        ioctl(s_fd, VIDIOC_QBUF, &buf);
        have_first = false;         // next pair starts fresh -- non-overlapping

        /* Book the pair. Committed together and only from the second pair on,
         * so all four accumulators cover exactly the same set of pairs and
         * ms_pair is comparable with ms_wait + ms_extract + ms_rest. The
         * remainder is the yield below plus preemption by anything above this
         * task -- it is not an error term to be explained away, it is the part
         * of the cycle this loop does not own. */
        int64_t t_end = esp_timer_get_time();
        if (s_last_pair_us) {
            s_us_cycle += (uint64_t)(t_end - s_last_pair_us);
            s_us_wait  += (uint64_t)pair_wait_us;
            s_us_ext   += (uint64_t)(t_ext1 - t_ext0);
            s_us_rest  += (uint64_t)(t_end - t_ext1);
            s_acct_pairs++;
        }
        s_last_pair_us = t_end;
        pair_wait_us = 0;

        if (s_flush_pairs > 0 && s_flush_dropped && --s_flush_pairs == 0)
            s_flush_done = true;

        /* ⚠ "Frames arrive faster than the diff can consume them, so this loop
         * never blocks on DQBUF" stood here until 2026-08-19 and is FALSE. The
         * accounting a few lines up measures it: at idle the loop sits ~14,6 ms
         * of every 56,0 ms pair blocked in DQBUF, because the sensor delivers
         * 36,1 fps and not the 50 the datasheet was read for. Believing the old
         * sentence cost a day: two changes that removed real CPU work from this
         * loop were predicted at ~22 % and measured 0,0 %, since a loop waiting
         * on frames absorbs a CPU saving without changing its rate.
         *
         * The yield still has to exist — the idle task would starve otherwise —
         * and thinning it is still right: vTaskDelay(1) sleeps to the next TICK,
         * 0..10 ms at CONFIG_FREERTOS_HZ = 100, ~5 ms average, and once per pair
         * that is ~1,4 ms of every pair at CAM_YIELD_PAIRS = 4. But it bought
         * 0,0 % of the bit rate, and the honest reason is that there was no
         * headroom to spend, not that the arithmetic was wrong.
         *
         * ⚠ Under measurement LOAD the picture inverts — extraction is preempted
         * and costs ~69,8 ms instead of 39,5, so the loop misses frames instead
         * of waiting for them. CPU spent here is not free during a session.
         *
         * So yield every CAM_YIELD_PAIRS pairs instead. ~200 ms between yields
         * is nowhere near the 5 s task watchdog, and every task that matters
         * runs ABOVE this one (elotto_task at 5, the network stack far higher);
         * only the idle task is below, and it has nothing to do but exist. */
        if (++yield_ctr >= CAM_YIELD_PAIRS) { yield_ctr = 0; vTaskDelay(1); }
    }
}

esp_err_t camera_init(void)
{
#if !CONFIG_ELOTTO_CAM_ENABLE
    ESP_LOGW(TAG_CAM, "disabled (CONFIG_ELOTTO_CAM_ENABLE=n) -- enable and set pins via "
             "idf.py menuconfig -> Elotto Camera (OV5647, Phase 0 entropy bring-up)");
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_ring = heap_caps_malloc(RING_WORDS * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        ESP_LOGE(TAG_CAM, "ring buffer alloc failed (%u bytes, needs PSRAM)",
                 (unsigned)(RING_WORDS * sizeof(uint32_t)));
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_ELOTTO_CAM_XCLK_PIN > 0
    esp_cam_sensor_xclk_config_t xclk_cfg = {
        .esp_clock_router_cfg = {
            .xclk_pin     = CONFIG_ELOTTO_CAM_XCLK_PIN,
            .xclk_freq_hz = CONFIG_ELOTTO_CAM_XCLK_FREQ,
        },
    };
    ret = esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &s_xclk_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CAM, "xclk allocate failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_cam_sensor_xclk_start(s_xclk_handle, &xclk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CAM, "xclk start failed: %s", esp_err_to_name(ret));
        return ret;
    }
#else
    ESP_LOGI(TAG_CAM, "XCLK pin disabled (-1) -- assuming the camera module supplies its own clock");
#endif

    static const esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port    = CONFIG_ELOTTO_CAM_SCCB_I2C_PORT,
                .scl_pin = CONFIG_ELOTTO_CAM_SCL_PIN,
                .sda_pin = CONFIG_ELOTTO_CAM_SDA_PIN,
            },
            .freq = CONFIG_ELOTTO_CAM_SCCB_I2C_FREQ,
        },
        .reset_pin = CONFIG_ELOTTO_CAM_RESET_PIN,
        .pwdn_pin  = CONFIG_ELOTTO_CAM_PWDN_PIN,
    };
    const esp_video_init_config_t cam_config = { .csi = &csi_config };

    ret = esp_video_init(&cam_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CAM, "esp_video_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (s_fd < 0) {
        ESP_LOGE(TAG_CAM, "open %s failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return ESP_FAIL;
    }

    // Use the sensor's default/native format (index 0) instead of hardcoding
    // a fourcc+resolution -- keeps this independent of the OV5647 Kconfig
    // format choice (RAW8 800x800 by default).
    struct v4l2_fmtdesc fmtdesc = { .index = 0, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(s_fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0) {
        ESP_LOGE(TAG_CAM, "ENUM_FMT failed");
        goto fail;
    }
    struct v4l2_frmsizeenum frmsize = { .index = 0, .pixel_format = fmtdesc.pixelformat };
    if (ioctl(s_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) != 0) {
        ESP_LOGE(TAG_CAM, "ENUM_FRAMESIZES failed");
        goto fail;
    }

    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width       = frmsize.discrete.width,
        .fmt.pix.height      = frmsize.discrete.height,
        .fmt.pix.pixelformat = fmtdesc.pixelformat,
    };
    if (ioctl(s_fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG_CAM, "S_FMT failed");
        goto fail;
    }
    s_frame_size = fmt.fmt.pix.sizeimage ? fmt.fmt.pix.sizeimage
                                         : (uint32_t)fmt.fmt.pix.width * fmt.fmt.pix.height;
    ESP_LOGI(TAG_CAM, "format " V4L2_FMT_STR " %ux%u size=%u",
             V4L2_FMT_STR_ARG(fmt.fmt.pix.pixelformat),
             (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height, (unsigned)s_frame_size);

    // Disable AEC/AGC (manual mode) and force fixed exposure/gain by direct
    // register write. The esp_cam_sensor OV5647 driver's public API only
    // exposes an "AE target" control (auto-exposure setpoint, not a manual
    // override) -- ESP_CAM_SENSOR_IOC_S_REG bypasses it. See PLAN Phase 0.
    cam_reg_write(0x3503, 0x03);   // bit1 AGC manual, bit0 AEC manual
    cam_reg_write(0x350a, ((uint32_t)CONFIG_ELOTTO_CAM_REG_GAIN >> 8) & 0x03);
    cam_reg_write(0x350b, (uint32_t)CONFIG_ELOTTO_CAM_REG_GAIN & 0xFF);
    cam_reg_write(0x3500, ((uint32_t)CONFIG_ELOTTO_CAM_REG_EXPOSURE >> 12) & 0x0F);
    cam_reg_write(0x3501, ((uint32_t)CONFIG_ELOTTO_CAM_REG_EXPOSURE >> 4) & 0xFF);
    cam_reg_write(0x3502, ((uint32_t)CONFIG_ELOTTO_CAM_REG_EXPOSURE << 4) & 0xF0);
    cam_verify_regs("after-write");

    struct v4l2_requestbuffers req = {
        .count  = CAM_BUF_COUNT,
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG_CAM, "REQBUFS failed");
        goto fail;
    }

    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index  = i,
        };
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG_CAM, "QUERYBUF[%d] failed", i);
            goto fail;
        }
        s_bufs[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, buf.m.offset);
        if (!s_bufs[i]) {
            ESP_LOGE(TAG_CAM, "mmap[%d] failed", i);
            goto fail;
        }
        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG_CAM, "QBUF[%d] failed", i);
            goto fail;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG_CAM, "STREAMON failed");
        goto fail;
    }

    cam_verify_regs("after-streamon");   // STREAMON rewrites sensor regs; confirm ours survived
    xTaskCreate(camera_task, "cam_task", 8192, NULL, ELOTTO_CAM_TASK_PRIO, NULL);
    ESP_LOGI(TAG_CAM, "streaming, extraction task started");
    return ESP_OK;

fail:
    close(s_fd);
    s_fd = -1;
    return ESP_FAIL;
#endif
}

bool camera_is_ready(void)
{
    bool ready;
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ready = s_stats.ready;
    xSemaphoreGive(s_mutex);
    return ready;
}

bool camera_read_word(uint32_t *out)
{
    if (!s_ring || !camera_is_ready()) return false;

    TickType_t waited = 0;
    const TickType_t limit = pdMS_TO_TICKS(CAM_STALL_TIMEOUT_MS);

    while (s_ring_tail == s_ring_head) {
        if (waited == 0) s_consumer_waits++;
        if (waited >= limit) {
            s_stalls++;
            return false;       // caller faults the node rather than hanging
        }
        vTaskDelay(1);          // wait for real bits; never invent them
        waited++;
    }

    *out = s_ring[s_ring_tail];
    s_ring_tail = (s_ring_tail + 1) % RING_WORDS;
    return true;
}

/* ── Calibration control (PLAN.md Task 1) ─────────────────────────────── */

void camera_stats_reset(int settle_pairs)
{
    if (settle_pairs < 1) settle_pairs = 1;
    s_settle_pairs = settle_pairs;       // capture task resets once it hits 0
}

bool camera_stats_settled(void)
{
    return s_settle_pairs == 0;
}

/* Apply exposure/gain and CONFIRM by read-back. Writing these registers is not
 * proof they stuck — the driver rewrites the format array on S_FMT and some
 * OV5647 revisions latch exposure only via group-hold — and a calibration that
 * silently failed to apply a setting would score the previous one and "choose"
 * it. Returns false if the sensor did not take the value. */
bool camera_set_exposure(uint32_t exposure, uint32_t gain)
{
    if (s_fd < 0) return false;
    if (exposure < 1) exposure = 1;
    // 0xFFFF, not 0xFFFFF: the three registers below hold 16 integer bits, so a
    // larger value loses its top bits on the way in and reads back as something
    // else — which this function would then report as "did not latch".
    if (exposure > 0xFFFF) exposure = 0xFFFF;
    if (gain > 0x3FF) gain = 0x3FF;

    cam_reg_write(0x3503, 0x03);                       // keep AEC/AGC manual
    cam_reg_write(0x350a, (gain >> 8) & 0x03);
    cam_reg_write(0x350b, gain & 0xFF);
    cam_reg_write(0x3500, (exposure >> 12) & 0x0F);
    cam_reg_write(0x3501, (exposure >> 4) & 0xFF);
    cam_reg_write(0x3502, (exposure << 4) & 0xF0);

    uint32_t re = 0, rg = 0;
    camera_get_exposure(&re, &rg);
    if (re != exposure || rg != gain) {
        ESP_LOGW(TAG_CAM, "exposure/gain did not latch: wrote %lu/%lu, read %lu/%lu",
                 (unsigned long)exposure, (unsigned long)gain,
                 (unsigned long)re, (unsigned long)rg);
        return false;
    }
    return true;
}

void camera_get_exposure(uint32_t *exposure, uint32_t *gain)
{
    uint32_t e0 = 0, e1 = 0, e2 = 0, g0 = 0, g1 = 0;
    cam_reg_read(0x3500, &e0); cam_reg_read(0x3501, &e1); cam_reg_read(0x3502, &e2);
    cam_reg_read(0x350a, &g0); cam_reg_read(0x350b, &g1);
    if (exposure) *exposure = ((e0 & 0x0F) << 12) | ((e1 & 0xFF) << 4) | ((e2 & 0xF0) >> 4);
    if (gain)     *gain     = ((g0 & 0x03) << 8) | (g1 & 0xFF);
}

bool camera_get_xor_fold(void)    { return s_xor_fold; }

void camera_ring_flush(int pairs)
{
    if (pairs < 1) pairs = 1;
    s_flush_done    = false;
    s_flush_dropped = false;
    s_flush_pairs   = pairs;      /* last, so the capture task sees a full request */
}

bool camera_ring_flushed(void) { return s_flush_done; }

double camera_fps_probe(int frames, int timeout_ms)
{
    if (frames < 2) frames = 2;
    if (frames > 200) frames = 200;
    s_probe_done = false;
    s_probe_fps  = 0.0;
    s_probe_req  = frames;
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        if (s_probe_done) return s_probe_fps;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_probe_req = 0;                 // give up; the task may still finish it
    return 0.0;
}

/* ── The sweep (PLAN.md Task 1) ────────────────────────────────────────────
 *
 * Exposure ladder, in sensor line units (the integer part of regs
 * 0x3500-0x3502). Geometric, and spanning BOTH sides of the power-on default
 * (16): the premise under test is that more light means more shot noise means a
 * more uniform LSB, and a ladder that only went up could not tell a real
 * response from a plateau. The Task 1 gate asks for exactly that curve — "if
 * bias does not respond to exposure, the premise is wrong and the task stops
 * there" — so the downward rungs are the control, not padding.
 *
 * Capped at 512 lines: past roughly the frame's own line count the sensor
 * stretches the frame instead of just integrating longer, and frame time is
 * what pays for the bit rate. Coarse on purpose — §1.5.1 budgets the whole
 * sweep at about 5 % of a ~10 min loop. */
static const uint32_t s_cal_ladder[] = { 4, 8, 16, 32, 64, 128, 256, 512 };
#define CAL_LADDER_N  (sizeof(s_cal_ladder) / sizeof(s_cal_ladder[0]))

/* Hard gates, inherited from the original Phase 0 gate these cameras have met
 * before. Quality first; rate is only the tie-break among candidates that pass. */
#define CAL_BIAS_TOL        1e-3
#define CAL_AUTOC_TOL       0.01
#define CAL_SIGMA_TOL       0.05
/* A window has to be long enough that the gate tests the SETTING and not the
 * sampling error of the window: SE(bias) = 0.5/sqrt(bits), so 2 Mbit gives
 * 3.5e-4 against a 1e-3 gate — about 3 sigma of headroom — and the 8 Mbit target
 * takes that to 1.8e-4 whenever the slice allows. A candidate too slow to reach
 * the minimum inside its slice is rejected as unmeasurable rather than scored on
 * numbers that cannot support the decision. */
#define CAL_MIN_BITS        2000000ULL
#define CAL_TARGET_BITS     8000000ULL
#define CAL_MIN_MINIRUNS    200
/* Over-illumination backstop. This was a LIGHT-LEAK floor at 64.0 (a quarter of
 * full scale) back when the cameras were meant to sit in the dark and any raised
 * mean_px meant uncontrolled light getting in. The enclosure is now deliberately
 * LIT (§1.13), so a high mean_px means the lamp is on, not that a leak
 * has appeared — the gate no longer measures what it was written to measure.
 *
 * Raised to 100.0 on measured evidence. At 64.0 it rejected the best rung the
 * rig has ever produced: exposure 64 at mean_px 68.7 gave bias -4.8e-5 (matching
 * the §1.9 open-bench best) with sigma 0.998 and autocorr 0.0009 — no quality
 * failure at all, refused on light level alone by 7%.
 *
 * The value sits inside a measured safe zone: mean_px 68.7 is clean, 118.6 is
 * decisively broken (sigma 1.91, autocorr 0.0097, bias -8.1e-3). 100 admits the
 * good rung and still stops gross over-illumination.
 *
 * Note what this gate does NOT protect against, because the answer is "nothing
 * the others miss": across every lit sweep measured, exposures 128/256/512 fail
 * BIAS and SIGMA on their own, and exposure 64 failed LIGHT *only*. Saturation
 * does not hide from the quality gates — it announces itself. Those are the real
 * protection; this is a coarse backstop. The measured value is reported per step
 * either way, so the choice stays auditable rather than implicit. */
#define CAL_MAX_MEAN_PX     100.0
/* ── The DARK end of the ladder (2026-08-19) ──────────────────────────────
 * CAL_MAX_MEAN_PX has had a partner missing since the enclosure was lit. The
 * premise of this instrument is that photons do the whitening (§1.13),
 * and the bottom rungs of the ladder do not have any: at exposure 4 the frame
 * sits at mean_px 3,1-3,3 and 14,5-17,4 % of pixel differences come back
 * exactly ZERO, against 6,9 % at exposure 128. A zero difference has a
 * deterministic LSB, so those rungs are not a dimmer version of the same
 * source, they are a partly frozen one.
 *
 * Measured on the 2026-08-19 four-node session, per-block offset by the rung
 * the sweep chose for that block:
 *
 *     exp     4       8      16      32      64     128
 *     off  -1,864  -0,781  -0,051  +0,093  -0,074  -0,039
 *
 * exp <= 8: -1,106 over 10 node-blocks (SE 0,267) against -0,030 over 106
 * (SE 0,032), t = -4,0. Five of the master's six |mean| > 1,2 blocks sat on
 * exposure 4 or 8, which is what put it soft-down for half that session.
 *
 * Both gates are cut BELOW the lowest rung that behaves (16: mean_px 5,3-5,4,
 * zero_diff 0,114-0,120) and above the highest that does not (8: 3,8-4,0 and
 * 0,130-0,151), and they are deliberately redundant -- mean_px is the physical
 * quantity, zero_diff is the mechanism, and a rung has to clear both. If the
 * lamp is ever dimmed these will start rejecting rungs that used to pass, which
 * is the correct behaviour and not a regression: the answer is light, not a
 * lower floor. */
#define CAL_MIN_MEAN_PX       5.0
#define CAL_MAX_ZERO_DIFF     0.125
/* ── What the bias gate is actually protecting (2026-08-19) ───────────────
 * CAL_BIAS_TOL is a bar on the wrong quantity. A run of `nseg` 200-bit segments
 * turns a bias b into a per-run z offset of
 *
 *     (b - 0,5) * GCP_SEGMENT_BITS * sqrt(nseg) / GCP_SEGMENT_SD
 *
 * = (b - 0,5) * 28,28 * sqrt(nseg). At the 52174 segments of a run=2 session
 * the 1e-3 constant therefore admits an offset of 6,5, and at run=5's 130435 it
 * admits 10,2 -- against a NODE_MEAN_SOFT of 1,5. The sweep was certifying
 * exactly the rungs the node-health gate then tripped on, and the same physical
 * camera passed or failed depending on ?run=.
 *
 * So the bar is stated as a z offset and converted with the segment count the
 * session will run (camera_cal_set_z_scale). Two limits keep it honest:
 *
 *  - never TIGHTER than CAL_BIAS_SE_K sigma of the window's OWN sampling error.
 *    SE(bias) = 0,5/sqrt(bits), which at the 4,8 Mbit a 10 s budget buys per
 *    rung is 2,3e-4 -- an implied z offset of +-1,5 at run=2. A gate below its
 *    own noise floor rejects at random, and a 1,0-z bar would be one. ⚠ This
 *    means the bias gate is NOISE-LIMITED, not tight: it cannot resolve the
 *    health bar it feeds. That is a property of the 10 s budget, and the way to
 *    change it is more bits per rung, not a smaller number here.
 *  - never LOOSER than CAL_BIAS_TOL, so this can only ever tighten the gate.
 *
 * Both limits are reported per step, so a sweep says which one bound it. */
#define CAL_MAX_Z_OFFSET      1.0
#define CAL_BIAS_SE_K         3.0
/* Frame pairs discarded before a window opens. Up to CAM_BUF_COUNT/2 pairs
 * already captured under the OLD setting can be sitting in the driver's queue,
 * so the discard count has to exceed that and then leave the sensor a few more
 * frames to actually apply the new one -- it is tied to the buffer count rather
 * than written out, because raising one and forgetting the other would let a
 * candidate be scored on its predecessor's frames. */
#define CAL_SETTLE_PAIRS    (CAM_BUF_COUNT / 2 + 2)
#define CAL_SETTLE_TIMEOUT_MS 4000

/* z per unit of bias for the session's run length; 0 = legacy fixed bar. */
static double s_cal_z_scale = 0.0;

void camera_cal_set_z_scale(double z_per_bias)
{
    s_cal_z_scale = (z_per_bias > 0.0) ? z_per_bias : 0.0;
}

/* The bias bar for ONE window, in bias units. See CAL_MAX_Z_OFFSET above. */
static double cal_bias_bar(uint64_t bits)
{
    if (s_cal_z_scale <= 0.0 || bits == 0) return CAL_BIAS_TOL;
    double bar = CAL_MAX_Z_OFFSET / s_cal_z_scale;
    double se  = 0.5 / sqrt((double)bits);
    if (bar < CAL_BIAS_SE_K * se) bar = CAL_BIAS_SE_K * se;  // never below its own noise
    if (bar > CAL_BIAS_TOL)       bar = CAL_BIAS_TOL;        // never looser than before
    return bar;
}

static uint32_t cal_gate(const camera_cal_step_t *s)
{
    uint32_t f = 0;
    if (s->bits < CAL_MIN_BITS || s->minirun_n < CAL_MIN_MINIRUNS) f |= CAM_CAL_FAIL_BITS;
    if (fabs(s->bias - 0.5) >= cal_bias_bar(s->bits)) f |= CAM_CAL_FAIL_BIAS;
    if (s->autocorr_max >= CAL_AUTOC_TOL)       f |= CAM_CAL_FAIL_AUTOC;
    if (fabs(s->sigma - 1.0) > CAL_SIGMA_TOL)   f |= CAM_CAL_FAIL_SIGMA;
    if (s->stuck_frames != 0)                   f |= CAM_CAL_FAIL_STUCK;
    if (s->mean_pixel_level >= CAL_MAX_MEAN_PX) f |= CAM_CAL_FAIL_LIGHT;
    /* The dark end. Both, and only when the window actually measured something:
     * a candidate that never got frames already fails BITS, and adding DARK to
     * it would report a light level that was never sampled. */
    if (s->bits >= CAL_MIN_BITS) {
        if (s->mean_pixel_level < CAL_MIN_MEAN_PX) f |= CAM_CAL_FAIL_DARK;
        if (s->zero_diff_frac > CAL_MAX_ZERO_DIFF) f |= CAM_CAL_FAIL_ZDIFF;
    }
    return f;
}

/* Measure one candidate. Returns false only if the sweep was aborted. */
static bool cal_step(camera_cal_step_t *st, uint32_t exposure, uint32_t gain,
                     bool fold, int64_t deadline_us, bool (*abort_cb)(void))
{
    memset(st, 0, sizeof(*st));
    st->exposure = exposure;
    st->gain     = gain;
    st->xor_fold = fold;

    if (!camera_set_exposure(exposure, gain)) {
        st->fail = CAM_CAL_FAIL_APPLY;   // read-back disagreed; score nothing
        return true;
    }
    s_xor_fold = fold;

    // Settle and flush BEFORE the window opens, or the score is a blend of two
    // settings: the driver still holds frames captured under the previous one
    // and the ring still holds words extracted from them.
    camera_stats_reset(CAL_SETTLE_PAIRS);
    int64_t settle_limit = esp_timer_get_time() + CAL_SETTLE_TIMEOUT_MS * 1000LL;
    while (!camera_stats_settled()) {
        if (abort_cb && abort_cb()) return false;
        if (esp_timer_get_time() > settle_limit) {
            // No frames are arriving. Clear the countdown by hand — leaving it
            // armed would make the capture task swallow the NEXT candidate's
            // pairs too, and every later step would score an empty window. Safe
            // here precisely because the capture task is demonstrably not
            // running the countdown down itself.
            s_settle_pairs = 0;
            st->fail = CAM_CAL_FAIL_BITS;
            ESP_LOGW(TAG_CAM, "cal: no frames at exposure=%lu -- candidate skipped",
                     (unsigned long)exposure);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    camera_stats_t cs;
    for (;;) {
        camera_get_stats(&cs);
        if (cs.bits_extracted >= CAL_TARGET_BITS) break;
        if (esp_timer_get_time() >= deadline_us) break;
        if (abort_cb && abort_cb()) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    camera_get_stats(&cs);

    st->bits             = cs.bits_extracted;
    st->minirun_n        = cs.sigma_samples;
    st->bias             = cs.bias;
    st->sigma            = cs.sigma;
    st->mbit_per_sec     = cs.mbit_per_sec;
    st->mean_pixel_level = cs.mean_pixel_level;
    st->zero_diff_frac   = cs.zero_diff_frac;
    st->stuck_frames     = cs.stuck_frame_count;
    double amax = 0.0;
    for (int L = 0; L < 4; L++)
        if (fabs(cs.autocorr_lag[L]) > amax) amax = fabs(cs.autocorr_lag[L]);
    st->autocorr_max = amax;
    st->fail = cal_gate(st);

    /* The bias bar MOVES with the session's run length now, so a logged verdict
     * cannot be checked without it -- and z_off is what the bar is really about:
     * the per-run offset this rung would contribute if it were chosen. */
    ESP_LOGI(TAG_CAM, "cal: exp=%-5lu gain=%-4lu fold=%d  bias=%.6f (%+.1e, bar %.1e, "
             "z_off %+.2f) sigma=%.4f r=%.4f mean_px=%.2f zero=%.4f %.3f Mbit/s  "
             "%.1f Mbit  %s0x%03x",
             (unsigned long)exposure, (unsigned long)gain, (int)fold,
             st->bias, st->bias - 0.5, cal_bias_bar(st->bits),
             (st->bias - 0.5) * s_cal_z_scale,
             st->sigma, st->autocorr_max,
             st->mean_pixel_level, st->zero_diff_frac, st->mbit_per_sec,
             st->bits / 1e6, st->fail ? "FAIL " : "pass ", (unsigned)st->fail);
    return true;
}

bool camera_calibrate(int budget_ms, bool (*abort_cb)(void), camera_cal_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->chosen = -1;
    out->z_scale = s_cal_z_scale;   // what the bias gate was scaled to, for the record
    if (s_fd < 0 || !camera_is_ready()) return false;

    int64_t t0 = esp_timer_get_time();

    // The setting in force at entry: the power-on default on loop 0, the
    // previous loop's choice afterwards. It is both the fallback and the first
    // rung of the sweep (see below).
    uint32_t e0 = 0, g0 = 0;
    camera_get_exposure(&e0, &g0);
    bool fold0 = s_xor_fold;

    if (budget_ms < 2000) budget_ms = 2000;
    // Steps: the entry setting, then the ladder. No fold trial — see below.
    int planned = 1 + (int)CAL_LADDER_N;
    if (planned > CAM_CAL_MAX_STEPS) planned = CAM_CAL_MAX_STEPS;
    int64_t slice_us = (int64_t)budget_ms * 1000 / planned;

    int n = 0;
    /* Step 0 re-measures the setting already in force, WITHOUT changing it.
     * Two things fall out of one slice. Against the same exposure appearing
     * later in the ladder it is the reset proof the Task 1 gate asks for — two
     * consecutive windows at the same setting must agree within sampling error.
     * And from loop 2 onward the entry setting is the previous loop's winner, so
     * this rung measures drift at a FIXED operating point, which is the only way
     * to tell a genuinely moving optimum from a noisy sweep. */
    if (!cal_step(&out->step[n], e0, g0, fold0, t0 + slice_us, abort_cb)) goto aborted;
    n++;

    for (int i = 0; i < (int)CAL_LADDER_N && n < planned; i++) {
        if (!cal_step(&out->step[n], s_cal_ladder[i], g0, fold0,
                      t0 + (int64_t)(n + 1) * slice_us, abort_cb)) goto aborted;
        n++;
    }

    /* NO FOLD TRIAL. The XOR fold was in scope as a processing parameter
     * (§1.7 decision 3) until the 10-loop session of 2026-07-26 measured
     * what fold-off actually does, and the decision was WITHDRAWN.
     *
     * The trial worked exactly as designed and that was the problem: fold-off
     * measured a bias of 0.500845 in its window, inside the 1e-3 gate, and won
     * the rate tie-break at 5.57 vs 3.37 Mbit/s. The master then ran a whole
     * loop on it and its per-run sigma went 1.043 -> 2.153, taking the combined
     * sigma from 1.041 to 1.382. The three slaves, still folded, stayed at
     * 0.97-0.99.
     *
     * The cause is that the unfolded LSB bias is NON-STATIONARY. Within that
     * same loop it moved from +8.4e-4 at calibration to -1.3e-3 during the
     * baseline minutes later — 2.1e-3 of travel against a 1e-3 gate. Neither
     * gate could see it: the bias gate reads one window, and the sigma gate
     * reads 3200-bit mini-runs *inside* that window, which are far too short to
     * show drift over seconds. Run-to-run sigma over 2.39 Mbit runs spread
     * across ten minutes is dominated by exactly that drift.
     *
     * So a 3 s window cannot certify fold-off; it can only get lucky. The fold
     * squares the bias away and is left permanently on, as Kconfig sets it. */
    out->nsteps = n;

    /* Selection: only gated candidates are eligible, LOWEST |bias-0.5| wins.
     *
     * This used to select the FASTEST passing candidate, from the assumption
     * that a shorter exposure means a faster frame rate means more bits per
     * second. That assumption is dead: §1.10 showed the bit rate is CPU-bound,
     * not exposure-bound, and a full sweep measures 3.217-3.293 Mbit/s across
     * exposure 4..512 — a 2.4% spread, i.e. measurement noise. So the tie-break
     * was comparing eight numbers that are all the same and picking whichever
     * happened to measure highest: a coin toss across the whole passing range.
     *
     * What that cost, measured: the master picked exposure 16 (bias -3.7e-4)
     * over 32 (-2.1e-4) on a 0.7% rate difference, and across one 5-loop
     * session its choice wandered 128 -> 256 -> 8 -> 128 -> 128, once landing
     * on a rung a standalone sweep had *failed* at -1.21e-3.
     *
     * Bias is the gate that actually binds and the property that costs sigma,
     * so select on it directly. It is noisy too (SE ~1.7e-4 per candidate), but
     * unlike rate it is INFORMATIVE: the real spread across a ladder is
     * -1.6e-3 to -4.8e-5, ~30x the SE, so this reliably picks the right region
     * and only ties arbitrarily between rungs that are genuinely equivalent.
     *
     * ── SIGMA MARGIN (added 2026-07-27, §1.17) ────────────────────
     * Bias alone is not enough, because bias is NOT the property that hurts.
     * A bias that survives calibration directly degrades the measurement, and
     * an over-dispersed node costs SNR and drives loop_sigma.
     *
     * Measured, over a 200-loop session: node .145 sat on exposure 32 in 91 of
     * 127 logged loops and produced every sigma excursion there (per-loop sigma
     * SD 0.245, max 3.008), while on exposure 16 it was indistinguishable from
     * a healthy node (SD 0.082 against the 0.089 expected from sampling alone).
     * Its exposure-32 rung sits ON the sigma gate: it passes some sweeps and
     * fails others, and when it passed, its bias often measured best — so the
     * bias-only rule selected it. The rule optimised the wrong quantity.
     *
     * Fix: a candidate must clear the sigma gate with MARGIN to be selectable,
     * not merely clear it. A rung that scrapes the gate is a rung on a cliff.
     * Half the tolerance is the threshold; among candidates that meet it, the
     * lowest |bias-0.5| still wins, so nothing else about the rule changes.
     *
     * Fallback to the bare gate if nothing meets the margin, so a node whose
     * whole ladder is marginal still chooses rather than keeping a stale
     * setting and reporting 'U'.
     *
     * ⚠ This narrows the window in which a cliff-edge rung can be picked; it
     * does not abolish it. The deeper problem is that the sweep's sigma does
     * not always predict the session's: .145's exposure 32 measured inside
     * [0.95, 1.05] often enough to be chosen 91 times, then produced 3.0 in
     * use. Verified not to disturb the healthy nodes — replayed against all
     * four measured ladders, master/.103/.155 keep exactly the rung they had. */
    int pick = -1;
    for (int pass = 0; pass < 2 && pick < 0; pass++) {
        const double stol = pass ? CAL_SIGMA_TOL : (CAL_SIGMA_TOL / 2.0);
        for (int i = 0; i < n; i++) {
            if (out->step[i].fail) continue;
            if (fabs(out->step[i].sigma - 1.0) > stol) continue;
            if (pick < 0 || fabs(out->step[i].bias - 0.5) <
                            fabs(out->step[pick].bias - 0.5))
                pick = i;
        }
    }

    uint32_t use_e   = (pick >= 0) ? out->step[pick].exposure : e0;
    uint32_t use_g   = (pick >= 0) ? out->step[pick].gain     : g0;
    bool     use_fld = (pick >= 0) ? out->step[pick].xor_fold : fold0;

    bool applied = camera_set_exposure(use_e, use_g);
    s_xor_fold = use_fld;
    if (!applied && pick >= 0) {
        // It latched while being scored and refuses now. Do not run the session
        // on an unverified setting: fall back to the entry one and say so.
        ESP_LOGE(TAG_CAM, "cal: chosen exposure=%lu would not re-apply -- reverting",
                 (unsigned long)use_e);
        camera_set_exposure(e0, g0);
        s_xor_fold = fold0;
        use_e = e0; use_g = g0; use_fld = fold0;
        pick  = -1;
    }

    out->chosen   = pick;
    out->ok       = (pick >= 0);
    out->exposure = use_e;
    out->gain     = use_g;
    out->xor_fold = use_fld;

    /* Report the measurements of the setting the camera is ACTUALLY on, whether
     * or not a gate certified it. When nothing passed we revert to the entry
     * setting, and step 0 measured exactly that — so its numbers are the honest
     * answer. Leaving the fields zeroed (as this first did) put `bias 0.000000`
     * into the /loops table, which reads like a catastrophic bias rather than
     * "not certified": a node that kept a perfectly good previous setting looked
     * broken. `ok` is what says certified or not; these are just what was seen. */
    int rep = (pick >= 0) ? pick : 0;
    if (rep < n && out->step[rep].bits >= CAL_MIN_BITS) {
        out->bias             = out->step[rep].bias;
        out->sigma            = out->step[rep].sigma;
        out->mbit_per_sec     = out->step[rep].mbit_per_sec;
        out->autocorr_max     = out->step[rep].autocorr_max;
        out->mean_pixel_level = out->step[rep].mean_pixel_level;
    }

    /* Open a clean window on the setting the session will actually run, so the
     * per-loop bias/rate in /status and /loops describe THIS loop's operating
     * point and not the last candidate the sweep happened to try. Waiting for it
     * also guarantees the first measured run draws from post-change frames. */
    camera_stats_reset(CAL_SETTLE_PAIRS);
    int64_t settle_limit = esp_timer_get_time() + CAL_SETTLE_TIMEOUT_MS * 1000LL;
    while (!camera_stats_settled() && esp_timer_get_time() < settle_limit) {
        if (abort_cb && abort_cb()) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    out->elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG_CAM, "cal: %s exposure=%lu gain=%lu fold=%d (%d/%d passed, %lu ms)",
             out->ok ? "chose" : "NO gated setting -- kept",
             (unsigned long)use_e, (unsigned long)use_g, (int)use_fld,
             pick >= 0 ? 1 : 0, n, (unsigned long)out->elapsed_ms);
    return out->ok;

aborted:
    out->nsteps = n;
    camera_set_exposure(e0, g0);
    s_xor_fold = fold0;
    // Re-arm rather than clear: nobody waits for it now, but the ring still holds
    // words extracted at whatever candidate the sweep died on, and the next
    // session must not draw them.
    camera_stats_reset(CAL_SETTLE_PAIRS);
    out->exposure   = e0;
    out->gain       = g0;
    out->xor_fold   = fold0;
    out->elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGW(TAG_CAM, "cal: aborted after %d step(s) -- entry setting restored", n);
    return false;
}

void camera_get_stats(camera_stats_t *out)
{
    if (!s_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_mutex);
}

/* ── GET /calibrate payload, shared by master and slave ─────────────────
 *
 * Moved here from the master's handler so both firmwares emit the same shape.
 * Takes void* rather than httpd_req_t* to keep esp_http_server out of this
 * component's public header — the .c includes it, callers pass their request
 * straight through.
 *
 * Chunked on purpose: 12 steps of ~200 bytes overflows any sane stack buffer,
 * and the whole point of serving it is that the per-candidate rows are the
 * diagnostic. The chosen rung alone cannot show whether bias responded to
 * exposure at all. */
esp_err_t camera_cal_send_json(void *httpd_req, const camera_cal_t *c)
{
    httpd_req_t *req = (httpd_req_t *)httpd_req;
    char buf[480];

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (!c || c->nsteps <= 0) {
        httpd_resp_sendstr(req, "{\"ran\":false,\"steps\":[]}");
        return ESP_OK;
    }

    int len = snprintf(buf, sizeof(buf),
        "{\"ran\":true,\"ok\":%s,\"chosen\":%d,\"exposure\":%lu,\"gain\":%lu,"
        "\"fold\":%d,\"bias\":%.6f,\"sigma\":%.4f,\"mbit_s\":%.3f,"
        "\"autocorr\":%.4f,\"mean_px\":%.2f,\"ms\":%lu,\"steps\":[",
        c->ok ? "true" : "false", c->chosen,
        (unsigned long)c->exposure, (unsigned long)c->gain, (int)c->xor_fold,
        c->bias, c->sigma, c->mbit_per_sec, c->autocorr_max,
        c->mean_pixel_level, (unsigned long)c->elapsed_ms);
    httpd_resp_send_chunk(req, buf, len);

    for (int i = 0; i < c->nsteps && i < CAM_CAL_MAX_STEPS; i++) {
        const camera_cal_step_t *s = &c->step[i];
        len = snprintf(buf, sizeof(buf),
            "%s{\"exposure\":%lu,\"gain\":%lu,\"fold\":%d,\"bits\":%llu,"
            "\"miniruns\":%d,\"bias\":%.6f,\"sigma\":%.4f,\"mbit_s\":%.3f,"
            "\"autocorr\":%.4f,\"mean_px\":%.2f,\"zero_diff\":%.4f,"
            "\"stuck\":%lu,\"fail\":%lu,\"pass\":%s}",
            i ? "," : "", (unsigned long)s->exposure, (unsigned long)s->gain,
            (int)s->xor_fold, (unsigned long long)s->bits, s->minirun_n,
            s->bias, s->sigma, s->mbit_per_sec, s->autocorr_max,
            s->mean_pixel_level, s->zero_diff_frac,
            (unsigned long)s->stuck_frames, (unsigned long)s->fail,
            s->fail ? "false" : "true");
        httpd_resp_send_chunk(req, buf, len);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── POST /expose payload, shared by master and slave ───────────────────
 *
 * See camera.h for why this lives here rather than in either firmware.
 *
 * The statistics are reset after the write, for the same reason the sweep resets
 * them per candidate: `camera_get_stats()` accumulates since the last reset, so
 * without it the mean_px the operator is tuning against would be a blend of the
 * old setting and the new one, converging on the truth over minutes. The whole
 * point of this endpoint is a live reading that responds to a screwdriver. */
esp_err_t camera_expose_handle(void *httpd_req, bool busy)
{
    httpd_req_t *req = (httpd_req_t *)httpd_req;
    char buf[256];

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (busy) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"err\":\"measuring -- abort first\"}");
        return ESP_OK;
    }
    if (!camera_is_ready()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"err\":\"camera not streaming\"}");
        return ESP_OK;
    }

    uint32_t exp_now = 0, gain_now = 0;
    camera_get_exposure(&exp_now, &gain_now);
    uint32_t want_e = exp_now, want_g = gain_now;

    char qry[96] = "", val[16] = "";
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
        if (httpd_query_key_value(qry, "exp", val, sizeof(val)) == ESP_OK)
            want_e = (uint32_t)strtoul(val, NULL, 10);
        if (httpd_query_key_value(qry, "gain", val, sizeof(val)) == ESP_OK)
            want_g = (uint32_t)strtoul(val, NULL, 10);
    }

    /* Same limits the Kconfig entries carry: 20-bit exposure, 10-bit gain, and
     * exposure 0 would stop integration altogether. camera_set_exposure() also
     * clamps the gain, but a rejected value should be visible in the reply
     * rather than silently corrected two layers down. */
    if (want_e < 1)       want_e = 1;
    if (want_e > 1048575) want_e = 1048575;
    if (want_g > 0x3FF)   want_g = 0x3FF;

    bool applied = camera_set_exposure(want_e, want_g);
    camera_stats_reset(CAL_SETTLE_PAIRS);

    uint32_t got_e = 0, got_g = 0;
    camera_get_exposure(&got_e, &got_g);
    snprintf(buf, sizeof(buf),
        "{\"ok\":%s,\"exposure\":%lu,\"gain\":%lu,\"asked\":%lu}",
        applied ? "true" : "false",
        (unsigned long)got_e, (unsigned long)got_g, (unsigned long)want_e);
    if (!applied) httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, buf);
    ESP_LOGI(TAG_CAM, "expose: set exposure=%lu gain=%lu -> read back %lu/%lu %s",
             (unsigned long)want_e, (unsigned long)want_g,
             (unsigned long)got_e, (unsigned long)got_g,
             applied ? "" : "(DID NOT LATCH)");
    return ESP_OK;
}

esp_err_t camera_selftest_handle(void *httpd_req, bool busy)
{
    httpd_req_t *req = (httpd_req_t *)httpd_req;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (busy) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"err\":\"measuring\"}");
        return ESP_OK;
    }

    /* The probe FIRST, while this task is only sleeping: it has to see an idle
     * CPU, or it measures the same contention the live loop already reports. */
    double fps_raw = camera_fps_probe(60, 6000);

    cam_selftest_t t;
    char buf[560];
    /* THE LIVE FRAME SIZE, not a convenient one. See extract.h. */
    if (!cam_extract_selftest(&t, s_frame_size)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"err\":\"no psram for the test buffers\"}");
        return ESP_OK;
    }
    /* ns_* are nanoseconds per PIXEL; cyc_* the same at the configured CPU
     * clock, which is the number that compares against "an XOR and a shift". */
    double mhz = (double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    snprintf(buf, sizeof(buf),
        "{\"equal\":%s,\"cases\":%d,\"failed_case\":%d,\"words\":%lu,"
        "\"ns_read\":%.3f,\"ns_ref\":%.3f,\"ns_fast\":%.3f,\"ns_stats\":%.3f,"
        "\"cyc_read\":%.1f,\"cyc_ref\":%.1f,\"cyc_fast\":%.1f,\"cyc_stats\":%.1f,"
        "\"speedup\":%.2f,\"cpu_mhz\":%d,\"bench_bytes\":%lu,"
        "\"frame_bytes\":%lu,\"ms_pair_ext\":%.1f,"
        "\"fps_raw\":%.2f,\"ms_pair_raw\":%.1f,"
        "\"popcount_ok\":%s,\"popcount_n\":%lu,\"popcount_bad\":\"%08lx\","
        "\"what\":%d,\"bad_at\":%lu,\"ref_w\":\"%08lx\",\"fast_w\":\"%08lx\","
        "\"ref_z\":%lu,\"fast_z\":%lu}",
        t.equal ? "true" : "false", t.cases, t.failed_case, (unsigned long)t.words,
        t.ns_read, t.ns_ref, t.ns_fast, t.ns_stats,
        t.ns_read * mhz / 1000.0, t.ns_ref * mhz / 1000.0,
        t.ns_fast * mhz / 1000.0, t.ns_stats * mhz / 1000.0,
        t.ns_stats > 0.0f ? t.ns_ref / t.ns_stats : 0.0, (int)mhz,
        (unsigned long)t.bench_bytes, (unsigned long)s_frame_size,
        /* What the benchmark says one live pair of extraction+statistics should
         * cost. Held next to /diagjson's measured ms_extract, this is the whole
         * comparison the harness exists to make. */
        t.ns_stats * (double)s_frame_size / 1e6,
        /* The sensor's own cadence with the CPU idle, and what one PAIR would
         * cost at it. If ms_pair_raw is close to the live ms_pair, the sensor
         * is the wall and no amount of faster extraction moves the bit rate. */
        fps_raw, fps_raw > 0.0 ? 2000.0 / fps_raw : 0.0,
        t.popcount_ok ? "true" : "false", (unsigned long)t.popcount_n,
        (unsigned long)t.popcount_bad,
        t.what, (unsigned long)t.bad_at,
        (unsigned long)t.ref_w, (unsigned long)t.fast_w,
        (unsigned long)t.ref_z, (unsigned long)t.fast_z);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}
