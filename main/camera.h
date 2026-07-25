#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Phase 0 (docs/PLAN_4NODE.md): OV5647 dark-frame noise bring-up + validation.
// Not wired into gcp_zscore_raw() yet — that swap-in is Phase 1.

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
    uint32_t stalls;              // reads that gave up and forced a TRNG fallback
} camera_stats_t;

// Bring up MIPI-CSI + OV5647, disable AEC/AGC, apply fixed exposure/gain from
// Kconfig, start capture task. Pin/register values MUST be set via
// `idf.py menuconfig` -> "Elotto Camera (OV5647) Configuration" to match the
// actual CSI wiring before this will do anything useful.
esp_err_t camera_init(void);

bool camera_is_ready(void);
void camera_get_stats(camera_stats_t *out);

// Phase 1 consumer API: pop one 32-bit word of extracted entropy.
// Blocks (vTaskDelay) while the ring is empty -- bits are never reused or
// fabricated to cover an underrun. Returns false only if the camera is not
// streaming or has produced nothing for CAM_STALL_TIMEOUT_MS, so the caller
// can degrade to the TRNG instead of hanging the session.
bool camera_read_word(uint32_t *out);
