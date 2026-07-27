#include <string.h>
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

// OV5647 dark-frame noise source (docs/PLAN_4NODE.md).
// Extraction: non-overlapping frame pairs, diff = f[2k+1] - f[2k] per pixel
// (cancels fixed-pattern noise exactly), LSB of each diff packed into a
// ring buffer, XOR-folded. camera_read_word() feeds noise_word() in sensor.c.
// SHARED between the master and slave repos -- a change here affects both nodes.

static const char *TAG_CAM = "cam";

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
    int ones = __builtin_popcount(w);
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
        s_autocorr_both1[L - 1] += (uint64_t)__builtin_popcount(w & (w << L));
        s_autocorr_pairs[L - 1] += (uint64_t)(32 - L);
    }
}

static void accumulate_pixel_level(const uint8_t *frame, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 16) {
        s_pixel_sum += frame[i];
        s_pixel_n++;
    }
}

// diff = b - a per pixel (mod 256), LSB packed. Non-overlapping: caller never
// reuses a or b as the other side of the next pair.
static uint32_t s_bitacc = 0;
static int      s_bitacc_n = 0;
static uint32_t s_fold_pending = 0;
static bool     s_fold_have = false;
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

static void diff_and_extract(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint8_t any_diff = 0;
    uint32_t zeros = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t d = (uint8_t)(b[i] - a[i]);
        any_diff |= d;
        if (d == 0) zeros++;
        uint32_t bit = d & 1u;

        // Runtime, not #if: the XOR fold is a *processing* parameter the
        // per-loop calibration is allowed to choose (PLAN.md Task 1). It halves
        // the rate to square away the raw LSB bias, so if a calibrated exposure
        // makes the raw bias pass on its own, turning the fold off doubles the
        // stream. Kconfig still sets the power-on default.
        if (s_xor_fold) {
            // Adjacent pixels are different Bayer channels, so folding them also
            // averages out per-channel LSB structure rather than just squaring a
            // single channel's bias.
            if (!s_fold_have) {
                s_fold_pending = bit;
                s_fold_have = true;
                continue;
            }
            bit ^= s_fold_pending;
            s_fold_have = false;
        }

        s_bitacc = (s_bitacc << 1) | bit;
        if (++s_bitacc_n == 32) {
            process_word(s_bitacc);
            s_bitacc = 0;
            s_bitacc_n = 0;
        }
    }
    s_zero_diffs += zeros;
    s_diff_n += n;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_stats.frame_pairs++;
    if (!any_diff) s_stats.stuck_frame_count++;
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
    s_bitacc = 0; s_bitacc_n = 0;
    s_fold_have = false;                 // a half-folded pair must not straddle
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
    s_stream_start_us = esp_timer_get_time();

    while (1) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
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
            accumulate_pixel_level(s_bufs[buf.index], s_frame_size);
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
            vTaskDelay(1);
            continue;
        }

        diff_and_extract(s_bufs[first.index], s_bufs[buf.index], s_frame_size);
        publish_stats();

        ioctl(s_fd, VIDIOC_QBUF, &first);
        ioctl(s_fd, VIDIOC_QBUF, &buf);
        have_first = false;         // next pair starts fresh -- non-overlapping

        // Frames arrive faster than a 640 KB PSRAM-to-PSRAM diff can consume
        // them, so this loop never blocks on DQBUF and would starve the idle
        // task (task watchdog). Yield once per frame: cheap relative to the
        // per-frame work, and the dropped frames cost nothing -- pairs are
        // non-overlapping anyway, and entropy is rate-limited by extraction.
        vTaskDelay(1);
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

void camera_set_xor_fold(bool on) { s_xor_fold = on; }
bool camera_get_xor_fold(void)    { return s_xor_fold; }

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
 * LIT (PLAN.md §1.13), so a high mean_px means the lamp is on, not that a leak
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
/* Frame pairs discarded before a window opens. CAM_BUF_COUNT=4 means up to two
 * pairs already captured under the old setting can be queued; the rest gives the
 * sensor a few frames to actually apply the new one. */
#define CAL_SETTLE_PAIRS    4
#define CAL_SETTLE_TIMEOUT_MS 4000

static uint32_t cal_gate(const camera_cal_step_t *s)
{
    uint32_t f = 0;
    if (s->bits < CAL_MIN_BITS || s->minirun_n < CAL_MIN_MINIRUNS) f |= CAM_CAL_FAIL_BITS;
    if (fabs(s->bias - 0.5) >= CAL_BIAS_TOL)    f |= CAM_CAL_FAIL_BIAS;
    if (s->autocorr_max >= CAL_AUTOC_TOL)       f |= CAM_CAL_FAIL_AUTOC;
    if (fabs(s->sigma - 1.0) > CAL_SIGMA_TOL)   f |= CAM_CAL_FAIL_SIGMA;
    if (s->stuck_frames != 0)                   f |= CAM_CAL_FAIL_STUCK;
    if (s->mean_pixel_level >= CAL_MAX_MEAN_PX) f |= CAM_CAL_FAIL_LIGHT;
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

    ESP_LOGI(TAG_CAM, "cal: exp=%-5lu gain=%-4lu fold=%d  bias=%.6f (%+.1e) sigma=%.4f "
             "r=%.4f mean_px=%.2f zero=%.4f %.3f Mbit/s  %.1f Mbit  %s0x%02x",
             (unsigned long)exposure, (unsigned long)gain, (int)fold,
             st->bias, st->bias - 0.5, st->sigma, st->autocorr_max,
             st->mean_pixel_level, st->zero_diff_frac, st->mbit_per_sec,
             st->bits / 1e6, st->fail ? "FAIL " : "pass ", (unsigned)st->fail);
    return true;
}

bool camera_calibrate(int budget_ms, bool (*abort_cb)(void), camera_cal_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->chosen = -1;
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
     * (PLAN §1.7 decision 3) until the 10-loop session of 2026-07-26 measured
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
     * squares the bias away and is left permanently on, as Kconfig sets it.
     * camera_set_xor_fold() stays for deliberate experiments; calibration no
     * longer touches it. */
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
     * ── SIGMA MARGIN (added 2026-07-27, PLAN.md §1.17) ────────────────────
     * Bias alone is not enough, because bias is NOT the property that hurts.
     * studentize() removes each loop's offset exactly, so a bias that survives
     * calibration is largely corrected downstream; an over-dispersed node is
     * not corrected — it just costs SNR, and it is what drives loop_sigma.
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
    char buf[320];

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
