# elotto – ESP32-P4 Project

## Environment
- Windows, drive D:\E-Lotto\elotto
- ESP-IDF at C:\esp\v6.0.1\esp-idf  (Tools: C:\Espressif)
- VS Code with Espressif ESP-IDF Extension
- Target: **esp32p4**
- Build system: idf.py via ESP-IDF Extension

## Project structure
- main/elotto.c   – app_main, Ethernet, webserver, HTML/JS UI, /diag + /loops endpoints
- main/sensor.c   – noise source, GCP analysis, baseline calibration, slave UART, lottery extraction
- main/sensor.h   – types and declarations
- components/elotto_camera/ – OV5647 dark-frame entropy (camera.c, include/camera.h, Kconfig).
  **Shared with the slave repo** via `EXTRA_COMPONENT_DIRS=../elotto/components` in
  elotto_slave/CMakeLists.txt — the two repos must stay siblings on disk, and a change here
  affects both nodes, so commit both repos together.

## Concept
Dual-ESP32-P4 system. Master (COM4) scores lottery numbers via GCP methodology. An optional
slave ESP32-P4 (COM6) measures in parallel via UART1 (GPIO14/15, 460800 baud); combined
z-score = (z_master + z_slave) / sqrt(2) (SNR x sqrt(2)).

**Noise source** (see docs/PLAN_4NODE.md — that file is the contract, and records every gate
result and design decision):
- Each node has its **own** OV5647 camera (never shared — sharing one would break
  independence by construction). Entropy = non-overlapping frame pairs, diff = f[2k+1]−f[2k]
  per pixel (cancels FPN exactly), LSB packed, XOR-folded. ~3 Mbit/s per node.
- `noise_word()` in sensor.c selects the source at runtime: `POST /start?src=1` = camera,
  `src=0` = on-chip TRNG (register 0x501101A4, still available for A/B). The UI has an
  **Entropy** dropdown; `/start` defaults to camera explicitly rather than inheriting the
  previous session's source.
- Segments per run are source-dependent: TRNG 32000 (6.4 Mbit), camera 8000 (1.6 Mbit ≈
  0.5 s). z stays N(0,1) either way — normalised by √segments.
- **A camera stall ABORTS the session** (`src_stalled`), never silently substitutes the
  TRNG: mixing sources mid-session would change the measured physics with no record of which
  runs were affected. Applies to a slave stall too (slave reports `T` in its `Z:` reply).
- **PSRAM is mandatory** with the camera (capture buffers + extraction ring). It also holds
  `g_status.loop_hist` — internal RAM is full with `results[]`, and adding a few KB of .bss
  fails the *link* (`--enable-non-contiguous-regions discards section …`), not the run.
- **Task priority is load-bearing**: the extraction task (`ELOTTO_CAM_TASK_PRIO` = 4) is
  CPU-hungry, so any task calling `camera_read_word()` must run *above* it or the producer
  starves the consumer (~10x slowdown; signature is ring `drops` huge with `waits == 0`).

Phase 1: baseline calibration (master + slave in parallel; informational — ranking uses
studentization instead).
Phase 0: score individual numbers 1..N, SCORE_REPS slave-combined runs each (currently **10**,
per-number SE = 1/√(2·REPS) ≈ 0.22; 40 → 0.11 when the pool choice must be trusted)
(score_one_run(), Stouffer), to build the pool.
Phase 2: measure all pool combinations in a fresh Fisher–Yates random order per loop
(s_perm[], drift immunity); results[] stays slot-indexed. A Runs cap stride-samples the full
space (slot i → combo ⌊i·full/total⌋). After each loop, studentize() re-expresses every z as
(z − loop mean)/loop σ (publishes loop_sigma, ideal ≈1) — bias correction + N(0,1) guarantee.
Master–slave independence: PairStats accumulates per-loop-centered (z_m, z_s) moments →
Pearson pair_r + sigma_m/sigma_s in /status (⚠ if |r|·√n > 3). Mid-loop aborts compact the
scattered partial measurements (compact_partial()).
Cross-loop drift (Phase 3): studentization removes each loop's own offset exactly, so only a
*trend across loops* survives it. record_loop() stores per-loop raw offsets/σ per node + camera
health (LoopStat, PSRAM, first LOOP_HIST=128 loops, served by **/loops**), and drift_add()
regresses the master's raw offset on the loop index using running sums (exact past 128) →
drift_slope / drift_t in /status; |t| > 3 flags real drift. The slave's per-loop camera numbers
come from the `D` command, queried once per loop while it is idle — a missing reply is
diagnostics-only and never drops the slave.
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

Claude builds and flashes **both** boards itself (master COM4, slave COM6) — see the
build/flash memory note for the exact invocation. Use the VS Code extension's Python venv
(`C:\Espressif\tools\python\v6.0.1\venv`), **not** `export.ps1`, or the build dir gets pinned
to a different interpreter and fails with "run 'idf.py fullclean'".

Check `http://192.168.178.100/status` before flashing the master — a flash resets it and
kills any running measurement session.

## Rules
- Never edit sdkconfig manually. To change a default, edit `sdkconfig.defaults`, delete
  `sdkconfig`, and let the build regenerate it (verify the diff afterwards).
- Target is always esp32p4
- The slave's `sdkconfig.defaults` must keep `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` **before**
  `ESP32P4_REV_MIN_0=y` — the latter depends on it, and without it the choice silently falls
  back to rev v3.1 and the binary refuses to boot on these v1.3 boards.
