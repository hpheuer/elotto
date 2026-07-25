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
    uint32_t stalls;              // reads that gave up and forced a TRNG fallback
} camera_stats_t;

// Bring up MIPI-CSI + OV5647, disable AEC/AGC, apply fixed exposure/gain from
// Kconfig, start capture task. Pin/register values MUST be set via
// `idf.py menuconfig` -> "Elotto Camera (OV5647) Configuration" to match the
// actual CSI wiring before this will do anything useful.
esp_err_t camera_init(void);

bool camera_is_ready(void);
void camera_get_stats(camera_stats_t *out);

// Priority of the extraction task created by camera_init().
#define ELOTTO_CAM_TASK_PRIO 4

// IMPORTANT — task priority: the extraction task is CPU-hungry (~7.6M pixel
// ops/s). The task calling camera_read_word() MUST run ABOVE
// ELOTTO_CAM_TASK_PRIO, or the producer starves the consumer and measurement
// slows by an order of magnitude while the ring sits permanently full
// (symptom: drops huge, waits == 0, runs 10x too long).
// The master's elotto_task is created at priority 5. The slave's command loop
// is app_main, whose priority IDF hardcodes to 1, so it calls
// vTaskPrioritySet(NULL, ELOTTO_CAM_TASK_PRIO + 1) at startup.
//
// Phase 1 consumer API: pop one 32-bit word of extracted entropy.
// Blocks (vTaskDelay) while the ring is empty -- bits are never reused or
// fabricated to cover an underrun. Returns false only if the camera is not
// streaming or has produced nothing for CAM_STALL_TIMEOUT_MS, so the caller
// can degrade to the TRNG instead of hanging the session.
bool camera_read_word(uint32_t *out);
