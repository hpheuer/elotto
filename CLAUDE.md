# elotto – ESP32-P4 Project

## Environment
- Windows, drive D:\E-Lotto\elotto
- ESP-IDF at C:\esp\v6.0.1\esp-idf  (Tools: C:\Espressif)
- VS Code with Espressif ESP-IDF Extension
- Target: **esp32p4**
- Build system: idf.py via ESP-IDF Extension

## v3.0+ (2026-08-02…03) — the single-pass session. READ THIS BEFORE THE REST.
**PLAN.md §2 is the contract; implemented 2026-08-02, run timing made configurable 2026-08-03.**
The core rule — no Focus item is ever measured twice — covers Phase 2: **every combination in the
confirmed pool is measured exactly ONCE**, in one Fisher–Yates random order, with **one continuous
window per Focus item** (scoring, baseline and measurement share the same length).
Consequences, stated once:
- **No loops, no loop counter, no Runs cap, no ranking modes.** `?loops=`, `?runs=`, `?rank=`
  answer **400**. The combobox, `RANK_*`, Stouffer/extreme accumulators, Coverage sets and the
  most-frequent row are deleted. The progress bar's 100 % is the full combination space
  (Euro 12+5 → 7920, 6-of-49 pool 15 → 5005; `NUM_RUNS` 8000 is the hard cap).
- **Measuring time is a session parameter** (UI: **Measuring Time (s)** · per Focus item;
  `?run=<s>` default **5**, `?gap=<s>` default **40 % of run**). Segment count is derived from a
  live cal (`RUN_SEGS_REF` / `RUN_MS_REF` in `sensor.h`). Actual wall time is **`focus_win_ms`** —
  long windows can stretch when the camera rate collapses under high duty cycle (that is the
  limit, not RAM). Stable live: **5 s request → ~4.7 s**, **7 s + 3 s gap → ~6.4 s**, zero stalls.
- **Stored z is RAW** — no studentize rewrite, no `zm = zraw − baseline_mean` (baseline is
  drift-reference ONLY now). Top/Bottom tables show **numeric two-sided p** from erfc(|z|/√2),
  not n.s. buckets. **Never pool v3 data with any v2.x session.**
- **Blocks replace loops as the statistics unit**: every `cal_interval_ms` (default
  **15 min**, `?calint=`, 0 = no mid-pass insertions) the pass parks for **sweep + baseline
  together**; the boundary closes a block → `/loops` row, drift point, pairwise fold.
  With raw z, **drift is the first diagnostic to read**.
- **`results[]` is in MEASUREMENT order** (`.index` = combination id, `.block` stamped), so the
  prefix is always complete: aborts need no compaction, and **`GET /results.csv?all=1`** streams
  every measured item (raw z) live mid-session. ⚠ RAM only — pull it periodically over a long
  pass; a master reboot loses unrepeatable measurements. **Bare `/results.csv` is the 15-row
  summary, NOT the record** — it cannot be re-derived into a pass (2026-08-08).
- Session wall time scales with `run`/`gap` and combination count. Attended by assumption;
  Pause stops the clock, Abort publishes the measured prefix.
- UI: **three tables of five** — Top-5 and Bottom-5 by raw z (`top[]`/`low[]`) and **Nearest-zero-5
  (`near[]`, 2026-08-08)**, plus significance line with `comparisons = items_done`, item counter +
  block badge, Save CSV → `/results.csv`.
  ⚠ **Nearest zero means nearest the PASS MEAN, not nearest raw 0.** Raw z carries the array's
  common offset (the 05-08 pass sat at −1.82), so |z_raw| ≈ 0 would select items ~1.8 σ *above*
  the array's own centre — the opposite of neutral. `results_near_mean()` picks by |z − mean|,
  which orders identically to the studentized |z − mean|/σ; `/status` publishes `pass_mean` and
  `pass_sigma` so the choice is checkable. The table shows a **Z\*** column and takes its p from
  that value, not from raw z.
  CSV is **German**: `;` separator, `,` decimal (user decision 2026-08-08) — a decimal point makes
  Excel read the whole column as text, so the separator alone would not have fixed anything.
  Firmware is delivered **over OTA only** in normal workflow (`POST /update`); `build.ps1`
  documents build, not serial flash.
Paragraphs below describing loops/ranking/Coverage or fixed 3.4 s windows are **historical**
where they contradict this section.

## Project structure
- main/elotto.c   – app_main, Ethernet, webserver, HTML/JS UI. Endpoints: `/` `/status` `/start`
  `/abort` `/loops` (per-**block** health) `/results.csv` (v3 export) `/focus` `/pause`
  `/calibrate` `/pool` `/ready` `/probe` `/expose`, plus
  **`/diag` (a four-camera health PAGE, live, one row per node)** and `/diagjson` (the master's own
  stats, which is where `/diag`'s JSON went on 2026-07-28). Slaves serve that same JSON at their
  own `/diag`.
  **`POST /expose?exp=<lines>[&gain=<g>]` sets one node's operating point by hand** — served by
  *every* node for its own camera (shared `camera_expose_handle()`, so four nodes cannot disagree
  about clamps or read-back), and driven from `/diag`'s per-row **&minus;/+** buttons, which halve
  or double the exposure. It resets the camera statistics, so `mean_px` answers in ~2 s — the point
  is tuning the physical LIGHT against a live reading. Refused **409** while measuring (session
  state on the master, `g_measuring` on a slave), and the reply carries the **read-back** setting,
  never the requested one. ⚠ **Not sticky**: the next calibration sweep overwrites it, which is
  correct — the sweep is what chooses a rung on evidence. Omitting `gain` keeps the gain in force.
  ⚠ `cfg.max_uri_handlers` must exceed **(count here) + 5 from elotto_ota**; registration past the
  cap fails and the return value is checked nowhere, so an endpoint just 404s silently.
- main/sensor.c   – GCP analysis, scoring/pooling, baseline, the single pass, blocks, drift,
  publishing (Top/Bottom-N, pass mean/σ)
- main/nodes.c    – **the array**: UDP link, discovery, calibration handshake, per-node health,
  the drop/reboot policy. Split out of sensor.c 2026-07-27 as a pure move (~520 lines); every
  static it owns was already used only by the functions that moved with it. `main/nodes.h` is
  the API. sensor.c reaches the other boards only through it.
- main/focus.c    – focus panel, pause, the run gap, and the session clock. These four
  share state, which is why they are one file: `pause_gate()` accumulates the time held,
  `elapsed_ms_now()` subtracts it, and a pause also nudges the gap timer so the break is not
  charged to `focus_gap_ms`. Split out 2026-07-27 as a pure move; it had **no** call into the
  GCP statistics at all. `main/focus.h` is the API.
- main/sensor.h   – types and declarations
- partitions.csv  – **shared** partition table (factory 1 MB + ota_0/ota_1 3 MB on 32 MB flash).
  Referenced by all three projects; a board flashed by one must be updatable by the others.
- ota_firmware/   – the network updater ("OTA-Firmware"), its own IDF project. Ethernet + HTTP
  + esp_ota only; no camera, no GCP (68 KB RAM vs the app's 421 KB static).
- components/elotto_camera/ – OV5647 dark-frame entropy (camera.c, include/camera.h, Kconfig).
- components/elotto_link/ – the UDP wire format between master and slaves
  (`EL1 <seq> <payload>`, ports 5000/5001). One definition compiled into both ends, so the
  two cannot disagree about framing.
- components/elotto_gcp/ – **the z-score primitive itself** (`gcp_zscore_raw()`), for the same
  reason and with more at stake: the combine is Sum(z)/√k over nodes, which is only meaningful
  if every node computes z identically. It was duplicated in `main/sensor.c` and
  `elotto_slave/main/slave.c` until 2026-07-27 — arithmetic identical, maintained twice. A
  divergence there would have been invisible: a wrong-but-plausible z looks exactly like a
  result. The two call sites differed only in abort handling, now the `on_yield` callback
  (slave polls its abort socket; master passes NULL and aborts between runs).
  ⚠ `GCP_SEGMENT_SD` is the literal `7.07106781`, **not** `sqrt(50.0)` — every z the rig has
  recorded came from that constant, and a change in the numbers must never be a side effect
  of tidying.
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

Sealed dark it measured badly, so **the box is now LIT** (`docs/PLAN_HISTORY.md` §1.13). Darkness cost
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

~~Two software policies cost a factor of 7 in bias~~ — **BOTH FIXED 2026-07-27/28**:
- ~~The `mean_px < 64` gate excludes the best rung.~~ **`CAL_MAX_MEAN_PX` is now 100.0.** It was a
  light-*leak* floor written when the cameras were meant to sit dark; the enclosure is deliberately
  lit, so a high `mean_px` means the lamp is on. 100 sits inside the measured safe zone (68.7 clean,
  118.6 broken). ⚠ It is now *binding on the master*, whose exposure-32 rung fails on light at
  104.65 with a perfectly good σ of 1.0394 — a live decision, see PLAN.md §1.16.
- ~~Calibration selects the *fastest* passing rung.~~ **It selects the lowest |bias − 0.5| among
  candidates that clear the σ gate WITH MARGIN** (|σ−1| ≤ half the tolerance), falling back to the
  bare gate if none qualify. The margin was added after a 200-loop session showed the bias-only
  rule putting `.145` on a rung it could not sustain in **91 of 127 loops**; see PLAN.md §1.17.
  ⚠ That result also **refutes** the earlier "per-loop calibration protects the tail" reading.

⚠ **Hardware state (2026-07-28):** all four are lit. **`.145` (slave1) is the weak node** — it has
a genuinely lower usable ceiling than its neighbours (clean to `mean_px` ≈34, broken by ≈46–60,
where `.155` is clean at 63–68), so **adding light makes it worse, not better** (PLAN.md §1.16).
Its LED also failed progressively on 2026-07-27 (light fell to 0.35×); after a physical repair it
sits at ~0.88× of its original level. **`.103` degrades under sustained load** — clean on idle
sweeps, per-loop σ SD 0.24 over 78 loops — which is CLAUDE.md open item 3, still unexplained.

**σ and the pairwise results ARE affected by sealing the box — and lighting it fixed them.**
✅ **Task 1's `?cal=0` control is DONE (§1.14).** Two matched 5×430 arms under light:
`cal=30000` gave σ **1.0040 ± 0.0144**, `cal=0` gave **0.9995 ± 0.0147** — difference **0.22 SE**,
so **per-loop calibration is statistically neutral**, the one way Task 1 could have done damage.

| | sealed dark (§1.12) | lit (§1.14) |
|---|---|---|
| mean per-run σ | 1.0795 ± 0.0222 | **1.0040 ± 0.0144** |
| worst \|r\|√n | 2.92 | **1.27** |
| master drift | −0.334 z/loop, t = −4.75 | −0.075 z/loop, t = −2.62 |

Thermal is the best explanation and is still **unproven** — enclosure temperature was never
measured — but three signatures agree across both conditions. ⚠ Arm A's pairwise matrix was lost by
starting arm B before capturing `/loops`+`/status`; a new session resets `PairAcc` and `loop_hist`.
**Capture both at arm completion, before starting anything else.**

All four boards are provisioned, each with its own OV5647, and all four run simultaneously:
**all four take PoE directly from one switch** (no splitters — PLAN_NETWORK Risk 2 resolved).

**Power topology is settled: all four on PoE, permanently (user decision, 2026-07-26).** The
master previously ran on separate USB power as the PLAN_NETWORK **Risk 1 control** — the idea
being that if the three PoE nodes correlated with each other but not with the master, the shared
rail was the mechanism. That split came off when the master's LED was fitted and **will not be
restored. Do not re-propose it.**

The consequence, stated once so it is not rediscovered as a surprise: **inter-node correlation can
no longer be attributed to the power rail versus anything else.** All four now share one rail, so
a rail effect and a genuine or environmental effect look identical. The one measurement that could
ever separate them is already in the bank — §1.12's arm A, which ran while the split was still
intact and found master↔slave pairs at mean +0.023 against slave↔slave +0.024, with the largest
single pair on the *isolated* node. That is the standing evidence that the correlation is **not
rail-borne**, and it cannot be repeated on this rig. Treat it accordingly.

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

**Plan**: `docs/PLAN.md` is the **live contract** — Task 1 is complete, so it holds §1.15
onward (what is open or recently measured). The design work and closed findings, **§1.1–§1.14**,
are in `docs/PLAN_HISTORY.md`. Numbering runs continuously across the two, so a citation like
"§1.13" resolves to the history file and "§1.16" to the live one.

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
- One source, **session segment count since v3.0+**: `g_status.run_segments` from `?run=`
  (default 5 s → segs via `RUN_SEGS_REF`/`RUN_MS_REF`) for baseline, Phase 0 scoring AND the
  measurement pass, all behind `g_status.gap_ms` (default 40 % of run). z stays N(0,1) at any
  length, being normalised by √segments.
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

Phase 1: baseline (all nodes in parallel), repeated at **every block insertion**.
**v3: the subtraction is GONE, not just inert.** `zm = zraw − baseline_mean` was deleted — with
raw z published it would have been a live, master-only correction (the RANK_EXTREME_RAW trap).
The baseline is now purely the **drift reference**: `LoopStat.base` per block, the independent
cross-check against the block's own master mean (`raw_m`, which is what `drift_add()` regresses
on now — at ~205 items/block that mean carries SE ≈ 0.07 z, finer than any baseline could).
**Default 10 runs** (`?baseline=` overrides); at the v3 run length that is ~44 s per insertion.
The UI bar is labelled **"Baseline — drift reference"**, not "Calibration": the camera exposure
sweep is a different phase, and having two things called calibration in one interface was
ambiguous. The `cal*` element ids are historical.
**Two attended gates, both opt-in on `POST /start?confirm=1`** (added 2026-07-27/28). The web UI
always sends it; **curl never does**, so scripted runs and the `?cal=0` control never block:
- **`PHASE_READY` — the observer gate.** After calibration and baseline (~2 min in which nothing is
  displayed), the session parks and shows a big **Start** button; `POST /ready` releases it. Scoring
  is the first phase whose bits are collected while a target is on screen, so it is where the
  protocol actually begins — starting it the instant the baseline ends catches the observer
  mid-thought. **No timeout**, deliberately: there is no sensible default action. During preparation
  the Focus panel reads "Preparing the system". The button carries the instruction ("Focus and
  concentrate on the numbers only") and releasing it holds **1 s dark** (`READY_SETTLE_MS`) before
  the first number — pressing it is itself an act of attention, and onset is the payload. The phase
  moves to `PHASE_SCORING` *before* that delay: a `/status` poll still seeing `ready` would re-raise
  the overlay over the screen the observer was just told to attend to.
- **`PHASE_POOL_CONFIRM` — pool confirmation.** After scoring, the proposed pool is published and
  the operator can edit it: `POST /pool?act=ok|more|cancel&main=..&euro=..`. "Select more" re-scores
  with the still-checked numbers **omitted** from the pass, so they keep the measurement that chose
  them. Combination counts recompute from the confirmed pool — keeping exactly 5+2 gives **ONE**
  combination, which is intended and is the highest-power way to use the instrument.
  ⚠ **15-minute timeout** accepts the proposal unchanged and records `pool_auto=1`. Verified firing.
  v3: this choice commits the whole single pass — there is nothing after it that re-scores.

Phase 0: score individual numbers 1..N with **exactly one node-combined run each, but a LONG
one** — the session window, `segments_for()`, default 5 s — sweeping the numbers in a **fresh
random order (Fisher–Yates, no repeats)** — `score_one_run()`. Per-number SE ≈ **0.29** at four
nodes. ⚠ `SCORE_SEGMENTS` no longer exists: the fixed ~3370 ms window became the operator-set
`?run=` in §1.16, and only `SCORE_GAP_MS` survives, as the **default blank of 2 s** (`focus.h`).
⚠ **Never repeat a target in place**, in any form. The phase has been through four shapes and two
were rejected on the same ground: 5 short reps (until 2026-07-25) froze the panel ~6.9 s and only
the first window had an onset; 3 short reps (briefly, 2026-07-30) reintroduced exactly that. One
long run gives the same arithmetic — a 3× run is Σdev/√(3N), i.e. the Stouffer combination of
three 1× runs — while keeping **one onset per number**, which is the payload.
⚠ **The gap scales with the window** — `?gap=`, default 40 % of `?run=`; every phase runs the
same window and the same gap. Duty cycle, not window length, is what starves the extraction task;
3.4 s behind a 350 ms blank would be ~90 %. Measured at the old fixed settings: window **3370 ms
± 1.5 ms across 62 numbers**, gap 1010 ms, duty 76.9 %, rate 3.37–3.38 Mbit/s (collapsed regime
is 2.68), zero stalls, `net_lost` 0. See §1.16 for the live cal at the configurable settings.
⚠ `LINK_MEAS_MS` is gone — the reply window is now `LINK_MEAS_MS_FOR(nseg)`. The old flat 4000 ms
was headroom for a 1 s run and a **deadline** for a 3.4 s one: every slave would still be
measuring when it expired, all would look silent, and after `NODE_MISS_LIMIT` they would be
DROPPED, leaving a solo session that still looked healthy. Deliberately generous (user decision):
a late drop costs nothing here, a false drop costs an arm of data measured at √(k−1) unnoticed.
Scoring runs once per session and costs 62 numbers × (window + gap) — ~4.6 min at the old fixed
3.4 s, ~9 min at the 5 s default. It changes only *which* numbers enter the pool, never the
statistics measured on them.
Phase 2 (v3): **ONE pass over the whole combination space** in one Fisher–Yates random order
(s_perm[], drift immunity), each item measured exactly once at the **same session window as every
other phase**. `results[]` fills in **measurement order** (`.index` = combination id, `.block`
stamped), so the prefix is always the complete record — no slot-indexing, no stride mapping, no
compaction on abort. z is stored **raw**; `publish_result()` maintains Top-N/Bottom-N after every
item and `comparisons = items_done` feeds the Bonferroni line. **Pass mean/σ are NOT accumulated**
— `results_near_mean()` recomputes them from the measured prefix when `/status` or the CSV asks,
because the mean moves as the pass proceeds and an item judged against an early mean would be
judged against a number that no longer exists.
Node independence: PairAcc[i][j] accumulates per-**block**-centered moments for EVERY node pair
(6 at n=4), only over runs where both nodes contributed → the full Pearson matrix plus per-node
σ in /status (⚠ if |r|·√n > 3). **Check per-block combined σ as well** — a pooled pairwise max
missed a growing correlation that σ caught (see PLAN_NETWORK's open finding).
Cross-block drift: with raw z published, a trend across the pass lands directly in the numbers —
random order stops it attaching to particular combinations but it still widens the extremes, so
this is the **first diagnostic to read**. `close_block()` → record_loop() stores per-block raw
offsets/σ per node + camera health (LoopStat, PSRAM, first LOOP_HIST=128 blocks, served by
**/loops**), and drift_add() regresses the master's per-block mean on the block index (running
sums, exact past 128) → drift_slope / drift_t in /status; |t| > 3 flags real drift; the UI shows
the drift line on the results screen. The slave's per-block camera numbers come from the `D`
command, queried at block close while it is idle — a missing reply is diagnostics-only.
Results in the browser UI: **Top-5, Bottom-5 and Nearest-zero-5** (served as `top[]`/`low[]`/
`near[]` in /status) plus the Bonferroni significance line. Save CSV navigates to `/results.csv`.
Coverage and most-frequent are gone. ⚠ The **studentized-view checkbox specified in PLAN.md §2.3
was never built** — the studentized value exists only as the `Z*` column of the nearest-zero
table. Do not cite it as a feature (noted 2026-08-08).

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
- **Every display window equals the session measuring time** (`?run=` / UI field; default 5 s)
  with intentional blank `?gap=` (default 40 % of run). Scoring, baseline and measurement share
  one cycle. Duty cycle is the property that matters: a short blank starves the camera extraction
  task (cliff past ~75–80 %). Check **`focus_win_ms` / `focus_gap_ms`** live — segment→wall ms is
  nonlinear under load.
- ⚠ `camera_get_stats()` is cumulative **since the last `camera_stats_reset()`**, i.e. since the
  last sweep — with the v3 default that is one block (~15 min). In a `?cal=0` session there is
  no reset and they are lifetime averages — a falling `mbit_s` is then not evidence of live
  degradation.

**Camera calibration (PLAN.md Task 1, done 2026-07-26; v3: per BLOCK)**: at every insertion the
master broadcasts `K<budget_ms>`, sweeps its own exposure ladder in parallel, and waits for every
node's `OK:<exp>,<gain>,<fold>,<bias>,<mbit_s>,<G|U>`. Each node keeps the setting with the
**lowest |bias − 0.5| among candidates clearing the σ gate with margin** (see above); if none
passes it keeps the one it had and reports `U`. The budget is a *cap*, not a target, so progress
must never be estimated against it — at the old 30 s cap a sweep measured **~24 s**.
⚠ **The default cap is 10 s** (5 s was too short: long warm run 2026-08-05 left all
nodes with cam_cal=0). This is NOT a 3× saving on a fixed
measurement: the cap is divided evenly over the 9 candidates, so each rung is now scored on about
a **third of the bits** (~1.1 s instead of ~3.3 s). Per-rung bias/σ are correspondingly noisier —
the per-candidate bias SE was ~1.7e-4 at 30 s — so rungs sitting near a gate boundary will flip
between sweeps more often, which is the failure §1.17's σ-margin rule exists to contain.
`?cal=<ms>` restores any budget without a reflash.
`POST /start?cal=0` turns it off — that is the matched control, and per-loop calibration is
**statistically neutral** (PLAN_HISTORY.md §1.14 and PLAN.md §1.15: A−B = 0.52 SE
over 6×430 runs per arm).
`GET /calibrate` serves the whole last sweep per candidate with the gate each failed — **on every
node, not just the master**, which is what makes a per-node optical fault diagnosable at all.
**Nodes land on different exposures on purpose** (different physical sensors, different light);
what they must still share is the segment count, which travels on the wire. Enclosed, they still
spread **128 / 32 / 64 / 512** inside one dark box — so the spread is the *sensors* differing, not
the light, and it is not something the enclosure was ever going to remove.
The chosen setting is recorded per block in `/loops` — mandatory, because a re-tune nobody
logged is indistinguishable from drift in the data.
⚠ **The trigger is TIME (2026-07-29, PLAN.md §1.18; v3 made it the block clock).** Default
**15 min**, `POST /start?calint=<ms>`; **0 = no mid-pass insertions** (v3 changed 0's meaning —
there is no loop left for "every loop" to mean). Thermal drift moves on wall-clock time, and the
insertion cadence is also what sets the drift regression's resolution. `LoopStat.cal_ms` is 0
when a sweep did not run, `/status` carries `cal_interval_ms` and `cal_did_sweep`, and the
`cam_*` fields always hold the setting in force.

**No loops, no ranking modes (v3).** The session is ONE pass; each item's z is its own single
raw measurement, so the four v2 ranking rules (`RANK_PEAK/CUMULATIVE/EXTREME/EXTREME_RAW`) had
nothing left to differ about and were deleted with their accumulators (`s_zsum`/`s_zmin`),
`publish_cumulative/extreme/coverage/frequency` and `absorb_loop`. `?rank=` (like `?loops=` and
`?runs=`) answers **400**. Top-10/Bottom-10 are updated after **every item**, so /status always
shows the live intermediate ranking; `comparisons = items_done` keeps the Bonferroni line honest
while the pass is still running.

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

**USB is only for:** a fresh board (bootloader + partition table + factory updater), or a node
whose recovery updater is gone. OTA cannot repair those. Example (replace COMx after listing ports):
`.\build.ps1 -C ota_firmware -p COMx erase-flash` then `... -p COMx flash`.
**COM ports are not stable** — never trust a number written in a table; list ports first.

| Action                  | VS Code shortcut |
|-------------------------|------------------|
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

## Where things stand (2026-08-08) — read this first

⚠ **THE HARDWARE WAS CHANGED on 2026-07-29 — lighting above all (user).** Every number in this
file and in PLAN.md dated before that describes an instrument that no longer exists: the exposure
ladders, `mean_px` ceilings, per-node biases, σ figures and the `.145` weak-node finding all have
to be re-measured. **Nothing recorded before the change may be pooled with anything recorded
after it.** The software, its gates and their reasoning are unaffected.

**`docs/data/` holds post-change sessions again** (it was emptied on 07-29 for the reason above):
two 07-30 ladder sets, the **2026-08-05 complete 6-of-49 pass** (5005 items, ~14 h) and the
**2026-08-08 pass aborted at 3404 items** with all four ladders. Both v3 passes are usable.

**The instrument is sound.** In a 200-loop session the master and `.155` had per-loop σ SD of
0.097 and 0.085 against the **0.089 that sampling alone predicts** — indistinguishable from ideal.
Task 1 (per-loop calibration) is closed and neutral. `main/` is split four ways (elotto/sensor/
nodes/focus) and the z-score primitive is shared with the slave via `components/elotto_gcp`, all
verified behaviour-neutral. **Every result so far is null**, which is what a working null
instrument produces.

**Open, in the order I would pick them up:**
1. **`.103`'s load degradation** — the only unexplained instrument fault. Clean on idle sweeps,
   per-loop σ SD 0.24 over 78 loops. The free diagnostic is to **swap `.103`'s camera with
   `.155`'s**: if the fault follows the camera it is the sensor, if it stays it is the board.
2. **The 08-05 pass's extremes are BLOCK-CLUSTERED — read this before ranking anything.** In the
   complete 5005-item 6-of-49 pass, all five top items fell in block 6 and all five bottom items
   in block 40. That is drift expressing itself as extremes, exactly as §2.3 predicted, not a
   per-combination effect: `drift_t` was **−4.34** (flag threshold 3) and the pass ran at mean
   **−1.82** with σ **2.19** — more than twice the σ ≈ 1 a clean combine should give, with `.155`
   at 1.494 and `.103` at 1.231 individually while all six pairwise |r| stayed ≤ 0.04. **σ, not
   correlation, is where this array fails**, which is the old open item 1's rule paying out.
   ⚠ That session also ran with `cal_budget_ms` 5000 and came out with **`cam_cal=0` on all four
   nodes** — no rung was certified all session. The default is back at 10 s (`673a86d`), so a
   repeat under a working sweep is the first thing to measure.
3. **`CAL_MAX_MEAN_PX` = 100 is binding on the master** (its exposure-32 rung fails on light at
   104.65 with σ 1.0394). A decision, not a measurement.
4. **Does calibration reduce the RATE of bad loops?** §1.14/§1.15 only ever asked "does it add
   variance?" (no). The tail question needs a count of excursions over many loops, not a mean.

**Deferred by the user — do not start unasked:** the attended-vs-unattended (focus) comparison.
**Dropped by decision — do not re-propose:** node-drop test, camera-fault/reboot path,
camera-stall abort, restoring the master to USB power.

⚠ **Do not switch camera hardware** on the theory that the OV5647 is the problem — the data says
otherwise (two nodes at sampling limits, autocorr 10–50× inside tolerance). The capture runs at
**RAW8 800×800**, ~13 % of the sensor, because the pipeline is PSRAM-bound at a 640 KB diff per
frame pair; more megapixels would *lower* the bit rate. If it is ever tried, buy **one** and run a
matched pair, not four.

## Old open items (2026-07-25) — closed, and what survives them

⚠ Compressed 2026-08-08. Every number below was measured on the **pre-07-29 optics**, so it
describes an instrument that no longer exists (see "Where things stand"). What is kept here is
the *reasoning* that still applies; the measurements live in `docs/PLAN_HISTORY.md`.

1. **Inter-node correlation growing during a session** — did not reproduce on the open bench
   (§1.10), came partially back under the sealed-dark box at σ 1.0795 ± 0.0222 with the worst
   pair at |r|√n = 2.92, i.e. *below* the flag threshold of 3 while σ ran to 1.15 (§1.12).
   **The standing lesson is the diagnostic rule, not the numbers: judge a session on per-block
   combined σ AND the full pairwise matrix, never on `pair_r` alone** — a pooled worst pair
   stayed silent through exactly this. The mechanism was never identified (best reading:
   thermal, unproven), so the ×√n gain is still **not established**.
2. **Node-drop test, camera-fault/reboot path, camera-stall abort** — **DROPPED BY DECISION
   (user, 2026-07-26), do not re-propose.** This is a research instrument, not a commercial
   application. The code exists and has been reasoned about: treat it as **unverified by
   choice**, not as an outstanding task.
3. **Camera bias degrades under sustained load** — **still open**, and the one item here that is
   not archaeology. `.103` went 0.499307 idle → 0.497884 after a session, outside the 1e-3 gate.
   Per-block calibration resets the statistics each block, so `/loops` numbers are per-window
   rather than lifetime. ⚠ A fresh idle-vs-loaded baseline must be taken on the **current**
   optics before any load effect can be read off — the *level* of bias moves with the light, so
   an old idle figure is not a valid reference. Related: the master's own bias level, which
   calibration largely explains by moving it off the Kconfig default of 16 lines.
4. **The Focus run window drifts** — did not reproduce; mechanism found. **The bit rate is
   CPU-bound, not exposure-bound**: exposures ran 4→512 across nodes simultaneously and the gap
   never moved. Only the XOR fold shifted it, by genuinely doubling one node's rate — one of the
   two reasons the fold is out of the calibration sweep. `focus_win_ms`/`focus_gap_ms` are
   measured in **every** session and recorded per block in `/loops`; check them after any
   camera/rate change, since segment→wall ms is nonlinear under load.

**Phase 5 (Focus display) is DONE** (2026-07-25). A matched no-focus control session was recorded
alongside it; the comparison itself — whether attention shows up in the statistics — is the
experiment, not a gate, and has not been analysed. It is **deferred by the user**.
