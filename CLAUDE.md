# elotto – ESP32-P4 Project

## Environment
- Windows, drive D:\E-Lotto\elotto
- ESP-IDF at C:\esp\v6.0.1\esp-idf  (Tools: C:\Espressif)
- VS Code with Espressif ESP-IDF Extension
- Target: **esp32p4**
- Build system: idf.py via ESP-IDF Extension

## Project structure
- main/elotto.c   – app_main, Ethernet, webserver, HTML/JS UI, /diag + /loops + /focus + /pause
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

✅ **DARK ENCLOSURE EXISTS (2026-07-26, later).** A LEGO dark box covers all four cameras. It is
verified dark by *flatness*, not by any single reading: across exposure 4→512 the master's
`mean_px` moves only 3.06→3.90 (**1.27× over a 128× integration range**), where open on the bench
it moved 2.6→140.8 (54×). The residual ≈3.1 is sensor black level plus read noise, not light.
Do not judge the enclosure by one `mean_px` — at exposure 16 it reads 3.52 enclosed vs 4.89 on the
morning bench, only 1.4× apart, which alone looks like a *partial* box. Sweep, don't spot-check.

Sealed dark it measured badly, so **the box is now LIT** (`docs/PLAN.md` §1.13). Darkness cost
raw LSB uniformity — photon shot noise had been doing real whitening work — and sealing it cost σ
as well. **"Controlled light" was always the goal; it is not the same as darkness.**

**Final master optics (2026-07-26 evening):** one LED on its **own supply**, steady DC, stable to
1.0 % over a minute. Chosen exposure 16 at `mean_px` 15.6, bias **−3.3e-4**, σ 1.005, autocorr
0.0005, `zero_diff` 9.2 % — the best sustained state the master has been in, and `zero_diff` now
beats the old open bench. `mean_px` spans 25.7× across the ladder (sealed dark gave 1.27×).

⚠ **NEVER power illumination from a node's VSYS pin.** An LED on VSYS with PWM dimming produced
bias −4.33e-3 and certified **0 of 9** rungs. The mechanism is **conducted, not optical** — PWM
current on the rail feeding the sensor's analog supply. It was misdiagnosed as optical flicker
first; the giveaway is that a separate supply restored 8 of 9 rungs *while `mean_px` barely
changed*. Also: LED output falls ~12 % as the junction warms, so let it settle before measuring.

Two software policies now cost a factor of 7 in bias, and neither is physics — see §1.13:
- **The `mean_px < 64` gate excludes the best rung.** Exposure 64 gives bias −4.8e-5 (matching the
  §1.9 open-bench best) with σ 0.998, rejected on light level alone by 7 %. Real saturation starts
  between `mean_px` 68.7 (clean) and 118.6 (σ 1.91). ⚠ **Not changed** — a decision, not a
  measurement. The earlier suggestion to *lower* it to 8.0 applied to the dark box and is dead.
- **Calibration selects the *fastest* passing rung, which is degenerate** now that §1.10 has shown
  the bit rate is CPU-bound (2.4 % spread across exposure 4→512). It should select **lowest
  |bias − 0.5|**. Dimming cannot fix this: selection always sits at the dim end of the passing range.

⚠ **Hardware state, unresolved:** only the **master** is lit — the three slaves are still sealed
dark, so the array is asymmetric — and **all four nodes are now on PoE**, so the Risk 1 control
below no longer exists and inter-node correlation is currently unattributable.

**σ and the pairwise independence results are not affected by light *level*** — they concern the
statistical behaviour of the combined z. They *were* affected by sealing the box: see §1.12, where
the sealed-dark 5-loop arm ran σ = 1.0795 ± 0.0222 against the open bench's 1.015 ± 0.012.

All four boards are provisioned, each with its own OV5647, and all four run simultaneously:
**the three slaves take PoE directly from one switch** (no splitters — PLAN_NETWORK Risk 2
resolved), while the **master stays on separate USB power on purpose**. That split is not
convenience, it is the Risk 1 control: if the three PoE nodes correlate with each other but not
with the master, the shared rail is the mechanism. Keep it that way.

⚠ **BROKEN as of 2026-07-26 evening: all four nodes are on PoE.** The master came off USB while
its LED was fitted. Until it goes back, there is no node on an independent rail and any inter-node
correlation is **unattributable**. This is not academic — it is exactly the control that let §1.12
show arm A's correlation was *not* rail-borne (master↔slave pairs mean +0.023 vs slave↔slave
+0.024, with the largest single pair on the isolated node). Restore it before the next session.

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
1.182) that a pooled pairwise check missed. Open finding (see item 1 below); judge
sessions on per-loop combined sigma AND the full pairwise matrix, never `pair_r` alone.

The UART1 crossover used before Phase C is gone: one datagram starts every node at once, where
N sequential UART writes would skew them. UDP loss is handled explicitly — every frame carries
the sequence number it answers, mismatches are dropped and counted (`net_stale`), and a
timed-out command is resent under the same sequence so a node replies from a one-entry cache
instead of measuring twice. **All receive timeouts go through `link_arm_timeout()`**: lwIP
rounds `SO_RCVTIMEO` to whole ms and treats 0 as *wait forever*, so a sub-millisecond remainder
would hang the session — this already happened once.

**Plan**: `docs/PLAN.md` is the current contract. Task 1 is per-loop camera calibration.

⚠ **`docs/PLAN_4NODE.md` and `docs/PLAN_NETWORK.md` were deleted (2026-07-26, user request).**
They are in git history, last present at commit `8e134e5` —
`git show 8e134e5:docs/PLAN_4NODE.md`. Source comments still cite them ("PLAN_4NODE Phase 3",
"PLAN_NETWORK §4"); those citations are historical and resolve to git history, not to a file on
disk. They were left as-is on purpose — repointing them at `PLAN.md` would make them cite phases
that document does not contain. Everything below is the standing summary of what they recorded,
so a fresh session does not need them.

**Noise source — PHOTONS ONLY (user decision, 2026-07-26)**:
- **The on-chip TRNG is REMOVED from both firmwares.** Not deselected — deleted. No
  `RNG_REG`, no `esp_random()`, no `?src=`, no Entropy dropdown, no TRNG tests in `/diag`.
  The reason is the GCP methodology: the claim under test is about a *physical* random
  source, and a whitened hardware RNG is an opaque digital post-process that would be
  indistinguishable from the real thing in every statistic this project computes. Keeping it
  as an A/B option meant the codebase could always, in principle, produce a result nobody
  could attribute. **Do not reintroduce it in any form.**
- Administrative randomness (the Fisher–Yates measurement order) uses an **xorshift32 PRNG
  seeded from the camera** once per session — `fast_rng()` / `prng_seed()` in sensor.c. It
  never enters a z-score; drawing it from the camera word by word would stall the session for
  bits that are never measured.
- Each node has its **own** OV5647 camera (never shared — sharing one would break
  independence by construction). Entropy = non-overlapping frame pairs, diff = f[2k+1]−f[2k]
  per pixel (cancels FPN exactly), LSB packed, XOR-folded. ~3 Mbit/s per node.
- One source ⇒ one segment count: `CAM_SEGMENTS` = 11950, ~1000 ms for every run — scoring,
  measurement and baseline alike. z stays N(0,1) regardless, being normalised by √segments.
- ✅ **The segment count travels on the wire** (`M<seg>`, `B<runs>,<seg>`), so the constant
  lives in `main/sensor.c` only. A slave that is *told* the length cannot disagree about it.
  `slave.c` keeps `CAM_SEGMENTS` **only** as the fallback for a pre-Phase-5 master, and logs
  loudly when it uses it. The yield/abort-poll cadence is `nseg/4` on both sides for the same
  reason — per-run wall time is max over nodes, so a mismatch slows every measurement to the
  slowest device.
- **A node whose camera stalls is REPORTED, DROPPED and REBOOTED.** There is nothing to fall
  back to by design. The node replies `E:<reason>` instead of `Z:<z>`; the master names it in
  `g_status.fault` (shown in `/status` and the UI in orange), drops it from the combine,
  bumps `nodes[].reboots` and sends `R` — the slave answers `OK` and calls `esp_restart()`.
  The camera is brought up in `app_main`, so a restart is the one recovery software has, and
  the node rejoins the *next* session by discovery, never the running one. The session only
  ABORTS (`src_stalled`) if the drop would leave fewer than two nodes.
  ⚠ **The master does not reboot itself** on its own camera failure — that would destroy the
  `/loops` history and the results the operator needs to see. It faults, reports and aborts.
- A run that dies part-way produces **no z at all**: `gcp_zscore_raw()` returns false rather
  than a short run, because a short run's z would be normalised by a √segments it never
  reached. A void baseline run is likewise not averaged in as a zero.
- **PSRAM is mandatory** with the camera (capture buffers + extraction ring). It also holds
  `g_status.loop_hist` — internal RAM is full with `results[]`, and adding a few KB of .bss
  fails the *link* (`--enable-non-contiguous-regions discards section …`), not the run.
- **Task priority is load-bearing**: the extraction task (`ELOTTO_CAM_TASK_PRIO` = 4) is
  CPU-hungry, so any task calling `camera_read_word()` must run *above* it or the producer
  starves the consumer (~10x slowdown; signature is ring `drops` huge with `waits == 0`).

Phase 1: baseline calibration (all nodes in parallel; informational — ranking uses
studentization instead).
Phase 0: score individual numbers 1..N with **exactly one** node-combined run each, sweeping the
numbers in a **fresh random order (Fisher–Yates, no repeats)** — `score_one_run()`. Repeats *in
place* were removed in Phase 5: five consecutive runs of the same number froze the Focus panel
for ~6.9 s, and onset is what the observer is meant to notice. Per-number SE = 1/√k ≈ 0.50 at
four nodes (was 0.22 at 5 reps), which changes only *which* numbers enter the pool, never the
Phase-2 statistics. If a pool choice must be trusted on its own, run several full random
passes — **never** repeats in place.
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

**Focus display (Phase 5)**: a "Focus:" card shows the current target in
large type for exactly the window its bits are collected in — the candidate number while
scoring, the whole draw while measuring — so the observer is present *while* the noise is
sampled (the original GCP/PEAR protocol). It changes nothing statistically, so a session is
merely **tagged**: `/start?focus=1`, `"focus"` in /status and `# focus=on|off` in the CSV.
An absent `?focus=` means unattended — a curl-started session has no observer. **Attended and
unattended sessions must never be pooled**; run matched no-focus controls.
- `GET /focus` (~60 B) is polled at **10 Hz** and is deliberately separate from the 2.5 KB
  /status: `seq` is monotonic per window, so the UI counts *missed* windows (a skipped window
  credits an effect to the wrong combination — mislabeling, not blur). `POST /pause?on=1|0`
  holds **between** runs only; state stays `running`, Σz and the permutation resume where they
  left off, and paused time is excluded from `elapsed_ms` (`paused_ms` records it).
- **Every focus display is 1000 ms**, scoring and measurement alike, with a uniform **350 ms**
  blank (`RUN_GAP_MS`) after every run including baseline. So `CAM_SEGMENTS` (11950) /
  `TRNG_SEGMENTS` are single constants — no phase split. 350 ms not 200 because conscious
  noticing smears over ~100–300 ms, so a 200 ms blank lets attention to target N overlap N+1's
  sampling; the hardware forces it too (1000 ms at a 200 ms gap is 83% duty, past the point
  where the loop starves the camera extraction task it consumes from and the rate collapses).
  Cycle ≈ 1.4 s, so `Runs` defaults to **430** for a ~10 min loop.
- ⚠ **The window is set by a segment count and the conversion is not stable** — the achievable
  window moved 1.75× across one afternoon under identical settings. Check
  `focus_win_ms`/`focus_gap_ms` in /status rather than assuming the constants still hold.
- ⚠ `camera_get_stats()` is cumulative **since the last `camera_stats_reset()`**. With per-loop
  calibration on (the default) that reset happens every loop, so `mbit_s`/`bias` in `/status`
  and `/loops` are now per-loop figures. In a `?cal=0` session there is no reset and they are
  lifetime averages again — a falling `mbit_s` is then not evidence of live degradation.

**Per-loop camera calibration (PLAN.md Task 1, done 2026-07-26)**: at the start of every loop the
master broadcasts `K<budget_ms>`, sweeps its own exposure ladder in parallel, and waits for every
node's `OK:<exp>,<gain>,<fold>,<bias>,<mbit_s>,<G|U>`. Each node keeps the fastest setting that
passes the quality gates; if none passes it keeps the one it had and reports `U`. `~27 s` per loop
(≈4.4 % of a 10 min loop). `POST /start?cal=0` turns it off — that is the matched control.
`GET /calibrate` serves the master's whole last sweep, per candidate, with the gate each failed.
**Nodes land on different exposures on purpose** (different physical sensors, different light);
what they must still share is the segment count, which travels on the wire. Enclosed, they still
spread **128 / 32 / 64 / 512** inside one dark box — so the spread is the *sensors* differing, not
the light, and it is not something the enclosure was ever going to remove.
The chosen setting is recorded per loop in `/loops` — mandatory, because a per-loop change nobody
logged is indistinguishable from drift in the data.

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
`POST /start` also returns **409** while a session runs (it used to answer "ok" and silently do
nothing, leaving the *previous* run's `loops`/`runs` in force — an ignored parameter that looked
exactly like a working one). Abort first, then start.

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

1. ~~**Inter-node correlation grows during a session.**~~ **DOES NOT REPRODUCE (2026-07-26).**
   The 10-loop calibrated session held combined σ at **1.015 ± 0.012** with every one of the six
   pairs at |r| ≤ 0.0145 over 4300 runs (worst |r|√n = 0.95 vs threshold 3). The original
   finding was σ 1.038 → 1.083 → 1.182 with a pooled worst pair of +0.064 — same array, same
   power topology (master on USB, three slaves on one PoE rail), 10× the runs.
   **Not yet proof the ×√n gain is established**: the mechanism behind the original growth was
   never identified, so a differing condition rather than a fix is still possible. But do not
   plan around the old numbers. See `docs/PLAN.md` §1.10.
   ⚠ **PARTIALLY BACK under the sealed-dark enclosure (§1.12).** A 5-loop arm ran σ =
   **1.0795 ± 0.0222** (3.6 SE above unity) with worst pair |r|√n = **2.92** — under the flag
   threshold of 3, so the pairwise check stayed silent while σ went to 1.15. Not the original
   *growth* pattern (it does not climb monotonically), and it is specific to the sealed box: the
   master's raw offset drifted −0.334 z/loop at t = −4.75, against −0.0054 at t = −0.20 on the
   open bench. Best reading is **thermal**, not electrical — see §1.12's rail test. Unresolved
   under the now-lit box, which has not been measured over a full session.
2. ~~**Node-drop test never run.**~~ **DROPPED BY DECISION (user, 2026-07-26) — do not
   re-propose.** The node-drop test (unplug a node mid-run, expect degrade to √3 with a UI flag
   and no crash) and the camera-fault/reboot path (`E:` reply → drop → `R` → `esp_restart()`)
   will **not** be tested. This is a research instrument, not a commercial application, and the
   cost of exercising these paths is not worth it here. The code exists and has been reasoned
   about; treat it as **unverified by choice**, not as an outstanding task.
3. **Camera bias degrades under sustained load** — `.103` went from 0.499307 idle to 0.497884
   after a session, outside PLAN_4NODE Phase 0's 1e-3 gate. Was thought to share a cause with
   (1); with (1) gone that link is dead. Per-loop calibration now resets the statistics every
   loop, so the numbers in `/loops` are per-window rather than lifetime, and every 10-loop
   per-node bias sat within 1e-3 of 0.5. **The enclosure now exists (see above), so the light
   confound is gone and this is finally chaseable** — but note §1.11: in the dark the *level* of
   bias is worse everywhere (best −4.6e-4 vs −7.8e-5 on the bench), so a fresh idle-vs-loaded
   baseline has to be taken under the enclosure before any load effect can be read off.

4. ~~**The Focus run window drifts.**~~ **DOES NOT REPRODUCE (2026-07-26).** Over the 10-loop
   calibrated session the window held **1102.0–1115.1 ms (spread 1.2 %)** and the gap
   **347.9–348.2 ms** against the 350 ms constant, across 4300 runs and 2 h 14 min. The original
   finding was 1.75× variation across a day and 8.3 % creep over 1700 runs.
   Also resolved with a mechanism: exposures ran from **4 to 512 across nodes simultaneously**
   and the gap never moved, because **the bit rate is CPU-bound, not exposure-bound**. Only the
   XOR fold shifted it, by genuinely doubling one node's rate — one of the two reasons the fold
   is out of the calibration sweep. `focus_win_ms`/`focus_gap_ms` are now measured in **every**
   session (attended or not) and recorded per loop in `/loops`.
5. **The master's camera bias is ~1.2e-3, outside Phase 0's 1e-3 gate** and ~9× the Phase 1
   figure — visible as a raw per-run offset of −2.69 z/run. `studentize()` removes it exactly
   and σ stayed ≈1 in both loops, so nothing downstream is affected. Related to item 3, now with
   a number. Phase 5 is not the cause: it *lowered* duty cycle from ~99.5% to 71%.
   **Largely explained, 2026-07-26**: the exposure sweep shows bias is a steep function of
   exposure, and the Kconfig default of 16 lines sat an order of magnitude worse than 128
   (−3.8e-4 vs −7.8e-5 in a clean 8 Mbit window). Per-loop calibration now moves it off 16. That
   does not explain the *load* dependence in item 3, only the level.
   ⚠ **Revised under the enclosure (§1.11): "a steep function of exposure" was a function of
   LIGHT.** In the dark the curve is shallow and U-shaped — 16 gives −1.14e-3 and the best rung
   (128) gives −4.6e-4, a factor of 2.5, not the order of magnitude seen on the bench. Calibration
   still moves the master off 16 and still helps; it just helps less than this item claims.

The **camera-stall abort has never been observed firing** (PLAN_4NODE's "Remaining work" item 1)
— Phase C proved the UDP abort path; this is the *source-loss* path. **Covered by the same
decision as open item 2: unverified by choice, not a task.**

**Phase 5 (Focus display) is DONE** (2026-07-25), gate passed except the 10 min loop budget
(10.5 min, item 4 above). A matched no-focus control session was recorded alongside it; the
comparison itself — whether attention shows up in the statistics — is the experiment, not a
gate, and has not been analysed.
