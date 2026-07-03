# elotto – ESP32-P4 Project

## Environment
- Windows, drive D:\E-Lotto\elotto
- ESP-IDF at C:\esp\v6.0.1\esp-idf  (Tools: C:\Espressif)
- VS Code with Espressif ESP-IDF Extension
- Target: **esp32p4**
- Build system: idf.py via ESP-IDF Extension

## Project structure
- main/elotto.c   – app_main, Ethernet, webserver, HTML/JS UI, /diag endpoint
- main/sensor.c   – TRNG, GCP analysis, baseline calibration, slave UART, lottery extraction
- main/sensor.h   – types and declarations

## Concept
Dual-ESP32-P4 system. Master (COM4) scores lottery numbers via GCP methodology using the
hardware TRNG (register 0x501101A4). An optional slave ESP32-P4 (COM6) measures in parallel
via UART1 (GPIO14/15, 460800 baud); combined z-score = (z_master + z_slave) / sqrt(2) (SNR x sqrt(2)).

Phase 1: baseline calibration (master + slave in parallel; informational — ranking uses
studentization instead).
Phase 0: score individual numbers 1..N, SCORE_REPS=5 runs each (Stouffer), to build the pool.
Phase 2: measure all pool combinations in a fresh Fisher–Yates random order per loop
(s_perm[], drift immunity); results[] stays slot-indexed. A Runs cap stride-samples the full
space (slot i → combo ⌊i·full/total⌋). After each loop, studentize() re-expresses every z as
(z − loop mean)/loop σ (publishes loop_sigma, ideal ≈1) — bias correction + N(0,1) guarantee.
Master–slave independence: PairStats accumulates per-loop-centered (z_m, z_s) moments →
Pearson pair_r + sigma_m/sigma_s in /status (⚠ if |r|·√n > 3). Mid-loop aborts compact the
scattered partial measurements (compact_partial()).
Results shown in browser UI (Ethernet, DHCP): two diversified Coverage sets — highest-z
(g_status.cover[]) and lowest-z (g_status.cover_low[]), via publish_coverage() greedy
max-spread (≤ nm/2 shared numbers per pick) — plus most-frequent from Z>2 runs and a
Bonferroni-corrected significance line (compute_significance()). The raw top[]/low[] rankings
are still computed (for significance) but no longer displayed or serialized. Coverage is
cumulative-mode only.

Loops: the whole experiment repeats N times (device-side, in elotto_task). Two ranking modes
(g_status.rank_mode): RANK_PEAK keeps the best single-run z across loops via absorb_loop();
RANK_CUMULATIVE (default) locks the pool after loop 0, re-measures the fixed combination set
each loop, accumulates s_zsum[] and ranks by Stouffer Z = Σz/√k (publish_cumulative(), no
in-place sort of results[] so the combo↔index mapping stays stable). Top-10/Bottom-10 +
frequency published after every loop so /status shows intermediate results. Optional Runs cap
(g_status.runs_limit) shortens Phase 2 for quick tests.

Modes: Eurojackpot (5 of 50 + 2 of 12, 7920 combinations) and 6 of 49 (5005 combinations).

## Build, Flash, Monitor
| Action                  | VS Code shortcut |
|-------------------------|------------------|
| Build + Flash + Monitor | F3               |
| Build only              | Ctrl+Shift+B     |
| Menuconfig              | Ctrl+E G         |

## Rules
- Never edit sdkconfig manually
- Target is always esp32p4
