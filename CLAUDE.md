# elotto – ESP32-P4 Project

## Environment
- Windows, drive D:\E-Lotto\elotto
- ESP-IDF at C:\esp\v6.0.1\esp-idf  (Tools: C:\Espressif)
- VS Code with Espressif ESP-IDF Extension
- Target: **esp32p4**
- Build system: idf.py via ESP-IDF Extension

## Project structure
- main/elotto.c   – app_main, Ethernet, webserver, HTML/JS UI, /diag + /loops endpoints
- main/sensor.c   – noise source, GCP analysis, baseline calibration, node link (UDP), lottery extraction
- main/sensor.h   – types and declarations
- partitions.csv  – **shared** partition table (factory 1 MB + ota_0/ota_1 3 MB on 32 MB flash).
  Referenced by all three projects; a board flashed by one must be updatable by the others.
- ota_firmware/   – the network updater ("OTA-Firmware"), its own IDF project. Ethernet + HTTP
  + esp_ota only; no camera, no GCP (68 KB RAM vs the app's 421 KB static).
- components/elotto_camera/ – OV5647 dark-frame entropy (camera.c, include/camera.h, Kconfig).
- components/elotto_link/ – the UDP wire format between master and slaves
  (`EL1 <seq> <payload>`, ports 5000/5001). One definition compiled into both ends, so the
  two cannot disagree about framing.
- components/elotto_ota/ – update endpoint + boot-safety logic (rollback, boot counter,
  mark-valid, /update /boot /reboot /poison /otainfo).
  All three components are **shared**: the slave repo pulls them via
  `EXTRA_COMPONENT_DIRS=../elotto/components` (elotto_slave/CMakeLists.txt) and ota_firmware
  pulls *only* elotto_ota by pointing at that single component directory — IDF compiles every
  component it discovers, so pointing at `components/` would drag the camera into the recovery
  image. The repos must stay siblings on disk; a change here affects several nodes, so build,
  flash and commit them together.

## Nodes (2026-07-25)
| node | IP | MAC | COM | flash contents |
|------|----|-----|-----|----------------|
| master | 192.168.178.100 | 80:f1:b2:d2:e3:1d | COM4 | factory = updater, ota_0/ota_1 = elotto app |
| slave0 | 192.168.178.103 (static lease) | 80:f1:b2:d2:e3:e5 | — | factory = updater, ota_1 = slave app |
| slave1 | 192.168.178.145 (static lease) | e8:f6:0a:e0:ce:a8 | — | factory = updater, ota_0 = slave app |
| slave2 | 192.168.178.155 | e8:f6:0a:e0:c7:a1 | COM9 | factory = updater, ota_0 = slave app |

All four boards are provisioned, each with its own OV5647, and all four run simultaneously:
**the three slaves take PoE directly from one switch** (no splitters — PLAN_NETWORK Risk 2
resolved), while the **master stays on separate USB power on purpose**. That split is not
convenience, it is the Risk 1 control: if the three PoE nodes correlate with each other but not
with the master, the shared rail is the mechanism. Keep it that way.

**COM ports are not stable** — the same slave has enumerated as COM6, COM8 and COM9. Always
list the ports before an `erase-flash` rather than trusting a number written down here; a wrong
port wipes a working node.

Node addresses are informational only — the master finds slaves by UDP broadcast, so a dynamic
lease works exactly like a static one and no IP table is maintained anywhere.

## Concept
Four-node ESP32-P4 array. The master scores lottery numbers via GCP methodology; up to three
slaves measure the same window in parallel, triggered by **one UDP broadcast** on the switch
(port 5000). Slaves are discovered by broadcast at every session start — no IP table, no node
count configured. Combined z = **Sum(z_node) / sqrt(k)** over the k nodes that answered *that
run*, so a node missing one reply costs that run's gain, not the session.

**The x sqrt(n) gain is NOT established** — it assumes the nodes are independent, and a 4-node
session showed inter-node correlation growing over ~30 min (combined sigma 1.038 -> 1.083 ->
1.182) that a pooled pairwise check missed. Open finding in docs/PLAN_NETWORK.md; judge
sessions on per-loop combined sigma AND the full pairwise matrix, never `pair_r` alone.

The UART1 crossover used before Phase C is gone: one datagram starts every node at once, where
N sequential UART writes would skew them. UDP loss is handled explicitly — every frame carries
the sequence number it answers, mismatches are dropped and counted (`net_stale`), and a
timed-out command is resent under the same sequence so a node replies from a one-entry cache
instead of measuring twice. **All receive timeouts go through `link_arm_timeout()`**: lwIP
rounds `SO_RCVTIMEO` to whole ms and treats 0 as *wait forever*, so a sub-millisecond remainder
would hang the session — this already happened once.

**Plans**: `docs/PLAN_4NODE.md` is the contract for the *noise source and statistics* (Phases
0–3 done, v2.1). `docs/PLAN_NETWORK.md` is the contract for *transport, provisioning and
firmware delivery* — 4-node UDP sync + Ethernet OTA, superseding PLAN_4NODE's UART-star
Phase 4. Both record every gate result and design decision.

**Noise source** (see docs/PLAN_4NODE.md):
- Each node has its **own** OV5647 camera (never shared — sharing one would break
  independence by construction). Entropy = non-overlapping frame pairs, diff = f[2k+1]−f[2k]
  per pixel (cancels FPN exactly), LSB packed, XOR-folded. ~3 Mbit/s per node.
- `noise_word()` in sensor.c selects the source at runtime: `POST /start?src=1` = camera,
  `src=0` = on-chip TRNG (register 0x501101A4, still available for A/B). The UI has an
  **Entropy** dropdown; `/start` defaults to camera explicitly rather than inheriting the
  previous session's source.
- Segments per run are source-dependent: TRNG 32000 (6.4 Mbit), camera 8000 (1.6 Mbit ≈
  0.5 s). z stays N(0,1) either way — normalised by √segments.
- ⚠ **`CAM_SEGMENTS` / `TRNG_SEGMENTS` are duplicated** in `main/sensor.c` and
  `../elotto_slave/main/slave.c` and **must match**, or the nodes integrate different windows
  and the whole premise (all nodes measure the *same* interval) fails silently — the numbers
  stay plausible. Change both, rebuild both, reflash **all four** nodes. Same for the
  yield/abort-poll cadence in `gcp_zscore_raw()`: per-run wall time is max over nodes, so a
  mismatch slows every measurement to the slowest device.
- **A node whose camera stalls is DROPPED**, never silently switched to the TRNG: mixing
  sources mid-session would change the measured physics with no record of which runs were
  affected. Dropping keeps the remaining nodes source-clean by construction; the session only
  ABORTS (`src_stalled`) if that would leave fewer than two nodes. Applies to a slave the same
  way (it reports `T` in its `Z:` reply).
- **PSRAM is mandatory** with the camera (capture buffers + extraction ring). It also holds
  `g_status.loop_hist` — internal RAM is full with `results[]`, and adding a few KB of .bss
  fails the *link* (`--enable-non-contiguous-regions discards section …`), not the run.
- **Task priority is load-bearing**: the extraction task (`ELOTTO_CAM_TASK_PRIO` = 4) is
  CPU-hungry, so any task calling `camera_read_word()` must run *above* it or the producer
  starves the consumer (~10x slowdown; signature is ring `drops` huge with `waits == 0`).

Phase 1: baseline calibration (all nodes in parallel; informational — ranking uses
studentization instead).
Phase 0: score individual numbers 1..N, SCORE_REPS node-combined runs each (**5**, per-number
SE = 1/√(k·REPS) ≈ 0.32 at two nodes, 0.22 at four; raise REPS when the pool choice must be
trusted on its own — it changes only which numbers enter the pool, not the Phase-2 statistics)
(score_one_run(), Stouffer), to build the pool.
Phase 2: measure all pool combinations in a fresh Fisher–Yates random order per loop
(s_perm[], drift immunity); results[] stays slot-indexed. A Runs cap stride-samples the full
space (slot i → combo ⌊i·full/total⌋). After each loop, studentize() re-expresses every z as
(z − loop mean)/loop σ (publishes loop_sigma, ideal ≈1) — bias correction + N(0,1) guarantee.
Node independence: PairAcc[i][j] accumulates per-loop-centered moments for EVERY node pair
(6 at n=4), only over runs where both nodes contributed → the full Pearson matrix plus per-node
σ in /status (⚠ if |r|·√n > 3). **Check per-loop combined σ as well** — a pooled pairwise max
missed a growing correlation that σ caught (see PLAN_NETWORK's open finding). Mid-loop aborts
compact the scattered partial measurements (compact_partial()).
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

**Build** — always through `build.ps1`, which sets the environment and forwards its arguments.
Shell state does not survive between tool calls, so bare `idf.py` needs the env re-exported
every time; and the script must use the **VS Code extension's** venv
(`C:\Espressif\tools\python\v6.0.1\venv`), not `export.ps1`'s, or the build dir gets pinned to
the wrong interpreter and fails with "run 'idf.py fullclean'".

```powershell
.\build.ps1 build                        # master
.\build.ps1 -C ota_firmware build        # updater  (-C selects another project)
.\build.ps1 -C ../elotto_slave build     # slave
```

**Flash — over Ethernet, not USB.** Firmware is pushed to the running node:

```powershell
curl http://192.168.178.100/update --data-binary @build/elotto.bin                       # master
curl http://192.168.178.103/update --data-binary @../elotto_slave/build/elotto_slave.bin  # slave
```

~750 KB in ~3 s. The node writes the *inactive* slot, reboots, and marks itself valid only
once its webserver answers, so a failed transfer or a dead image cannot strand it.
`/update` returns **409** while a measurement runs — no need to check `/status` first, though
`/status` still shows `fw_version`, `fw_sha` and `fw_slot` to confirm what is actually running.

**USB is now only for:** the partition table, the bootloader, or a node whose recovery updater
is gone. Those are the cases OTA cannot repair — the P4 has no
`SOC_RECOVERY_BOOTLOADER_SUPPORTED`. Installing the updater on a fresh board:
`.\build.ps1 -C ota_firmware -p COM6 erase-flash` then `... -p COM6 flash`.

| Action                  | VS Code shortcut |
|-------------------------|------------------|
| Build + Flash + Monitor | F3               |
| Build only              | Ctrl+Shift+B     |
| Menuconfig              | Ctrl+E G         |

## Rules
- Never edit sdkconfig manually. To change a default, edit `sdkconfig.defaults`, delete
  `sdkconfig`, and let the build regenerate it (verify the diff afterwards). Every project has
  a `sdkconfig.defaults` and sets `IDF_TARGET` in its CMakeLists — without both, a regenerate
  loses settings or fails with "CMAKE_C_COMPILER not set".
- Target is always esp32p4
- The slave's `sdkconfig.defaults` must keep `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` **before**
  `ESP32P4_REV_MIN_0=y` — the latter depends on it, and without it the choice silently falls
  back to rev v3.1 and the binary refuses to boot on these v1.3 boards.

## Open items (2026-07-25) — deferred by decision, not forgotten

1. **Inter-node correlation grows during a session.** Combined σ rose 1.038 → 1.083 → 1.182
   over three loops while the pooled worst pairwise r stayed a harmless-looking +0.064. The
   ×√n gain rests on independence, so this decides whether four nodes beat one. The array is
   already wired as the control (master isolated on USB, three slaves on one PoE rail) and
   `/status` publishes the full pairwise matrix, so one 3-loop session answers it: slaves
   correlating only with each other ⇒ the rail; the master tracking them too ⇒ the room.
   Full record in docs/PLAN_NETWORK.md.
2. **Node-drop test never run** (PLAN_NETWORK Phase D gate): unplug a node mid-run, expect
   degrade to √3 with a UI flag and no crash. The code path exists and is untested.
3. **Camera bias degrades under sustained load** — `.103` went from 0.499307 idle to 0.497884
   after a session, outside PLAN_4NODE Phase 0's 1e-3 gate. Likely the same cause as (1).

Also long-open: the **camera-stall abort has never been observed firing** (PLAN_4NODE's
"Remaining work" item 1) — the only safety claim in these documents that has only been reasoned
about. Phase C proved the UDP abort path; this is the *source-loss* path.
