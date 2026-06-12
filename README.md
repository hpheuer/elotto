# E-Lotto — GCP Analysis on ESP32-P4

ESP32-P4 project that generates Eurojackpot and 6-of-49 lottery numbers using the hardware
TRNG and [GCP methodology (Global Consciousness Project)](https://grokipedia.com/page/Global_Consciousness_Project).

## Screenshots

<table>
<tr>
<td align="center"><b>Measurement running</b></td>
<td align="center"><b>Results with Top-10 + Most frequent</b></td>
</tr>
<tr>
<td><img src="docs/screenshot_laufend.png" width="390"></td>
<td><img src="docs/screenshot_ergebnis.png" width="390"></td>
</tr>
</table>

## Hardware

- **Master (COM4):** Waveshare ESP32-P4-ETH — webserver, GCP, Eurojackpot/6-of-49
- **Slave (COM6):** second Waveshare ESP32-P4-ETH — GCP + UART1 handler only
- **PHY:** IP101GRI via RMII (Ethernet RJ45, DHCP) — master only
- **CPU:** ESP32-P4 @ 360 MHz, 768 KB SRAM
- **Chip revision:** v1.3 (sdkconfig adjusted: `CONFIG_ESP32P4_REV_MIN_0=y`)
- **UART1 connection:** Master GPIO14 → Slave GPIO15 (TX→RX), Master GPIO15 ← Slave GPIO14 (RX←TX), GND↔GND, 460800 baud

## Concept

**Goal: filter out the best number *combinations* by scoring them with the GCP algorithm.**
The combinations whose parallel TRNG stream deviates most strongly from chance — the highest
baseline-corrected **Z-scores** — are surfaced as the suggested lottery numbers.

Each **GCP run** reads 200,000 TRNG values directly from the hardware register:
- **32,000 segments** of 200 bits each
- Z-score per segment: `(ones − 100) / √50`
- Run Z-score: `Σ(Z_segment) / √32,000`, **corrected by the baseline mean**

### Program flow

A job runs three phases, optionally repeated over several **loops**:

1. **Baseline calibration** (`PHASE_BASELINE`) — N runs measure the TRNG's systematic bias;
   master and slave calibrate in parallel.
2. **Number scoring** (`PHASE_SCORING`) — every individual candidate number gets one GCP run.
   The highest-scoring numbers form a small candidate **pool**.
3. **Combination measurement** (`PHASE_MEASURING`) — every combination of the pool is
   enumerated lexicographically and measured with its own GCP run, then ranked by Z-score.
   The **Top-10** combinations are the result.

| Mode | Candidate pool | Combinations / loop |
|---|---|---|
| 6 of 49 | best **15** of 49 | C(15,6) = **5005** |
| Eurojackpot | best **12** of 50 + best **5** of 12 | C(12,5) × C(5,2) = 792 × 10 = **7920** |

**Loops** repeat the whole three-phase experiment N times — each loop runs a *fresh*
baseline, scoring and measurement. The cumulative **global Top-10** across all loops is
carried forward and shown live after every loop, so a strong combination found early
survives to the end. The **most frequent** numbers are aggregated across **all** loops'
runs with Z > 2.

## Web Interface

Accessible in the browser via Ethernet after startup (read IP from Serial Monitor).

| Element | Description |
|---|---|
| **Baseline runs** | Calibration runs per loop, default 100 (10–5000) |
| **Loops** | How often the whole experiment repeats, default 1 (1–50) |
| **Runs (0=all)** | Cap on measured combinations per loop for quick tests, `0` = all |
| **Euro-Lotto** | 5 numbers (1–50) + 2 bonus numbers (1–12) |
| **6 of 49** | 6 numbers (1–49) |
| **🔁 Loop X / N** | Loop counter, shown while running when Loops > 1 |
| **Calibration phase** | Gold progress bar with ✔ when done |
| **Number scoring phase** | Blue progress bar with ✔ when done |
| **Measurement phase** | Green progress bar with runtime, ETA and ✔ when done |
| **Top-10** | Best combinations by Z-score; updates live after each loop |
| **Most frequent** | Most frequent numbers across all Z>2 runs |
| **Abort** | Stops after current run, shows cumulative Top-10 so far |
| **Save CSV** | Downloads the displayed (merged) Top-10 as `.csv` |
| **Load previous CSV** | Load earlier CSVs and merge them into the ranking |
| **Browser reload / close** | ESP keeps running all loops; page reconnects and shows live progress |
| **Diagnostics** | `http://<IP>/diag` — compares register vs esp_random() |

## Key Code

### 1 — Direct TRNG Register Access

Instead of `esp_random()` (which goes through an internal driver), the hardware register
is read directly — **75× faster**, identical quality:

```c
// sensor.c
#define RNG_REG  (*((volatile uint32_t *)0x501101A4UL))
static inline uint32_t fast_rng(void) { return RNG_REG; }
```

### 2 — GCP Z-Score with `__builtin_popcount`

Per 200-bit segment, 6×32 + 1×8 = 200 bits are read with 7 TRNG reads.
`__builtin_popcount` counts the ones in one clock cycle instead of a 32-bit loop
(**28× less CPU work** per segment):

```c
// sensor.c — gcp_zscore_raw()
for (int seg = 0; seg < 32000; seg++) {
    int ones = __builtin_popcount(fast_rng())   // 32 bits
             + __builtin_popcount(fast_rng())
             + __builtin_popcount(fast_rng())
             + __builtin_popcount(fast_rng())
             + __builtin_popcount(fast_rng())
             + __builtin_popcount(fast_rng())
             + __builtin_popcount(fast_rng() & 0xFF);  //  8 bits
    z_sum += (ones - 100.0) / 7.07106781;  // sqrt(50) ≈ 7.071
}
return z_sum / sqrt(32000.0);
```

### 3 — Dual-ESP: Combined Z-Score (SNR ×√2)

Both ESPs measure simultaneously. The combined Z-score increases SNR by factor √2:

```c
// sensor.c — elotto_task() measurement loop
if (use_slave) uart_write_bytes(SLAVE_UART, "M\n", 2);  // start slave
double z = gcp_zscore_raw() - g_status.baseline_mean;   // master measures in parallel
if (use_slave) {
    double zs = slave_measure();                          // read slave Z
    if (s_slave_ok) z = (z + zs) * 0.70710678;           // ÷√2, SNR ×√2
}
```

Baseline calibration also runs in parallel: `slave_baseline_start()` sends `B<n>\n`
before the master loop, `slave_baseline_wait()` reads `OK\n` afterward — both run concurrently.

UART protocol (ASCII, 460800 baud):
```
P\n       → OK\n          Ping (startup)
B<n>\n    → OK\n          Baseline (n runs, blocks slave)
M\n       → Z:<float>\n   Measure (master + slave in parallel)
A\n       → OK\n          Abort
```

### 4 — Two-Phase Measurement (Baseline Correction)

The TRNG has a systematic bias of approx. −0.022 per segment.
Over 32,000 segments this accumulates to **Z ≈ −3.95 per run** without correction.
Solution: Phase 1 measures the bias, Phase 2 subtracts it:

```c
// sensor.c — elotto_task()

// Phase 1: Calibration
g_status.phase = PHASE_BASELINE;
double bsum = 0.0;
for (int i = 0; i < baseline_total; i++) {
    bsum += gcp_zscore_raw();
    g_status.baseline_done = i + 1;
}
double baseline_mean = bsum / baseline_total;

// Phase 2: Bias-corrected measurement
g_status.phase = PHASE_MEASURING;
for (int i = 0; i < runs_total; i++) {
    double z = gcp_zscore_raw() - baseline_mean;   // ← correction
    g_status.results[i].z_score = z;
}
```

### 5 — Number Scoring → Candidate Pool

Numbers are **not** drawn randomly. Every candidate number is GCP-scored with one run; the
highest-scoring numbers form the pool that combinations are later built from:

```c
// sensor.c — score_and_build_pool()
for (int k = 1; k <= max_val; k++)
    scores[k] = gcp_zscore_raw();          // one GCP run per number 1..max_val
// keep the pool_size highest scores, then insertion-sort the pool ascending
```

### 6 — Combination Enumeration & Ranking

Phase 2 enumerates **every** combination of the pool lexicographically (no randomness),
GCP-scores each, and ranks them by Z-score:

```c
// sensor.c — elotto_task() Phase 2
for (int i = 0; i < runs_total; i++) {
    int mi = i % main_combos, ei = i / main_combos;   // lexicographic index
    nth_combination(pool_main, pool_nm, nm, mi, g_status.results[i].nums);
    if (euro) nth_combination(pool_euro, 5, 2, ei, g_status.results[i].euro);
    g_status.results[i].z_score = gcp_zscore_raw() - g_status.baseline_mean;
}
qsort(g_status.results, runs_total, sizeof(RunResult), cmp_desc);   // rank by Z desc
```

### 7 — Multi-Loop Accumulation + Frequency

Each loop's results are folded into a cumulative **global Top-10** carry, and the Z>2
frequency histogram is accumulated across all loops. Both are published after every loop
so `/status` can show intermediate results, not only at the end:

```c
// sensor.c — absorb_loop()
qsort(g_status.results, done, sizeof(RunResult), cmp_desc);
for (int i = 0; i < done; i++) {
    if (g_status.results[i].z_score <= 2.0) break;   // sorted descending
    z2++;
    for (int j = 0; j < nm; j++) fm[g_status.results[i].nums[j]]++;
}
// merge this loop's top-N with the running carry, keep the global best TOP_N,
// then publish carry → g_status.top[] and most-frequent → g_status.freq_nums[]
```

## Insights from Development

### TRNG Register is 75× Faster than esp_random()

The diagnostics (`/diag`) showed:

```json
{"reg_ms":3, "reg_bias":0.499220, "reg_stuck":0, "reg_z_mean":-0.0221,
 "esp_ms":225, "esp_bias":0.499310, "esp_stuck":0, "esp_z_mean":-0.0195,
 "speedup":75.0}
```

- No stuck values (reg_stuck: 0) — no correlations
- Bit bias: 0.499220 instead of ideal 0.500000 — tiny but measurable deviation
- **Critical:** without baseline correction the bias produces systematically Z ≈ −3.95 per run

### Baseline Correction is Mandatory

The systematic hardware bias accumulates over 32,000 segments:

```
E[z_run] = -0.0221 × √32,000 ≈ -3.95 per run
```

Solution analogous to the eTensor project (Princeton PEAR lab methodology):
1. **Phase 1:** N calibration runs → determine `baseline_mean`
2. **Phase 2:** Measurement runs, each corrected: `z_corrected = z_raw - baseline_mean`

This gives each measurement an expected value of 0 — statistically correct.

### TRNG Register Address was Initially Biased

Direct access to register `0x501101A4` produced **exclusively positive Z-scores** in an
early test (all 50 runs > 0). Likely cause: TRNG initialization state on very first start.
After full IDF boot and with baseline correction the register works correctly.

Temporarily `esp_random()` was used — correct results, but 75× slower.

### Timing Benchmarks (200,000 values/run, ESP32-P4 @ 360 MHz, direct register)

| Config | Calibration | Measurement | Total |
|---|---|---|---|
| 100 baseline + 1000 runs | ~20 s | ~3 min | **~3 min** |
| 100 baseline + 4000 runs | ~20 s | ~13 min | **~14 min** |
| 100 baseline + 7000 runs | ~20 s | ~26 min | **~27 min** |
| 1000 baseline + 7000 runs | ~3 min | ~26 min | **~29 min** |

For comparison with `esp_random()` (75× slower): 1000 runs ≈ 4 hours.

### Optimizations

- **`__builtin_popcount`** instead of 200-bit loop: 28× less CPU work per segment
- **Direct TRNG register** instead of `esp_random()`: 75× faster (TRNG-limited)
- **Baseline correction**: eliminates hardware bias, statistically correct Z-scores
- **Number scoring + combination enumeration**: candidates are GCP-ranked, not randomly drawn
- **Multi-loop accumulation**: cumulative global Top-10 across loops, published live

### RAM Limit

`RunResult` occupies ~40 bytes. **Maximum: ~8000 runs** (320 KB result array).
Enforced in UI. ESP32-P4 has 768 KB SRAM.

### Chip Revision v1.3

Bootloader error on first flash: `requires chip revision [v3.1 - v3.99]`.  
Fix: `idf.py menuconfig` → Component config → ESP32P4-Specific →
Minimum Supported ESP32-P4 Revision → v0.0

## Build & Flash

```powershell
# IDF terminal (desktop shortcut "IDF_v6.0.1_Powershell")
cd D:\E-Lotto\elotto
idf.py build
idf.py flash -p COM4
idf.py monitor -p COM4
```

## Diagnostics

```
http://<IP>/diag
```

Compares direct TRNG register with `esp_random()`: speed, bias,
correlations, Z-score distribution. Runtime approx. 5 seconds.

## Environment

- ESP-IDF v6.0.1 (`C:\esp\v6.0.1\esp-idf`)
- Tools: `C:\Espressif` (EIM standard on this system)
- Target: `esp32p4`, chip rev v1.3

## Project Structure

```
main/
  elotto.c    — app_main, Ethernet, webserver, HTML/JS incl. /diag, CSV Save/Load, loop UI
  sensor.c    — GCP analysis, TRNG register, baseline, number scoring, combination
                enumeration, multi-loop accumulation, slave UART
  sensor.h    — types, ElottoStatus (phase/baseline/loop/top fields)
docs/
  screenshot_laufend.png   — web UI during measurement
  screenshot_ergebnis.png  — web UI with Top-10 + most-frequent result
build.ps1     — build helper script for standard PowerShell
sdkconfig     — ESP-IDF configuration

elotto_slave/main/
  slave.c     — slave GCP handler, UART1 protocol (P/B/M/A commands), timestamps in log
```

## Version History

| Version | Description |
|---|---|
| v1.0 | GCP webserver, Eurojackpot + 6-of-49, live progress, abort, Top-10 |
| v1.1 | Browser reconnect: page restores state after reload |
| v1.2 | 200K TRNG values/run, popcount optimization, configurable runs (max 8000) |
| v1.3 | Direct TRNG register (75× faster) + baseline calibration, /diag endpoint |
| v1.4 | Button grid layout, most-frequent row (Z>2), abort text, checkmarks |
| v1.5 | Dual-ESP: slave via UART1 (460800 baud), combined Z-score (÷√2, SNR ×√2), parallel baseline |
| v1.6 | CSV save/load in browser, parallel slave baseline, JS fix (buttons) |
| v1.7 | Multi-loop runs: cumulative global Top-10, live intermediate results after each loop, loop counter, `Runs` cap for quick tests; device-side loop (browser-independent); docs updated to reflect number-scoring + combination-enumeration flow |
