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
#include "camera.h"

// Phase 0 (docs/PLAN_4NODE.md): OV5647 dark-frame noise bring-up + validation.
// Extraction: non-overlapping frame pairs, diff = f[2k+1] - f[2k] per pixel
// (cancels fixed-pattern noise exactly), LSB of each diff packed into a
// ring buffer. Not wired into gcp_zscore_raw() -- that is Phase 1.

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
#if CONFIG_ELOTTO_CAM_XOR_FOLD
static uint32_t s_fold_pending = 0;
static bool     s_fold_have = false;
#endif

static void diff_and_extract(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint8_t any_diff = 0;
    uint32_t zeros = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t d = (uint8_t)(b[i] - a[i]);
        any_diff |= d;
        if (d == 0) zeros++;
        uint32_t bit = d & 1u;

#if CONFIG_ELOTTO_CAM_XOR_FOLD
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
#endif

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

    xTaskCreate(camera_task, "cam_task", 8192, NULL, 4, NULL);
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
            return false;       // caller degrades to TRNG rather than hanging
        }
        vTaskDelay(1);          // wait for real bits; never invent them
        waited++;
    }

    *out = s_ring[s_ring_tail];
    s_ring_tail = (s_ring_tail + 1) % RING_WORDS;
    return true;
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
