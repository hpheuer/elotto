# elotto – ESP32-P4 Project

## Environment
- Windows, drive D:\E-Lotto\elotto
- ESP-IDF at C:\esp\v6.0.1\esp-idf  (Tools: C:\Espressif)
- VS Code with Espressif ESP-IDF Extension
- Target: **esp32p4**
- Build system: idf.py via ESP-IDF Extension

## v3 — the single-pass session. READ THIS BEFORE THE REST.
**PLAN.md §2 is the contract.** The core rule: **every combination in the confirmed pool is
measured exactly ONCE**, in one Fisher–Yates random order, with **one continuous window per Focus
item** (scoring, baseline and measurement share the same length).

- **No loops, no loop counter, no Runs cap, no ranking modes.** `?loops=`, `?runs=`, `?rank=`
  answer **400**. The progress bar's 100 % is the full combination space (Euro 12+5 → 7920,
  6-of-49 pool 15 → 5005; `NUM_RUNS` 8000 is the hard cap).
- **Measuring time is a session parameter** (UI: **Measuring Time (s)** · per Focus item;
  `?run=<s>` default **5**, `?gap=<s>` default **40 % of run**). Segment count is derived from a
  live cal (`RUN_SEGS_REF` / `RUN_MS_REF` in `sensor.h`). Actual wall time is **`focus_win_ms`** —
  long windows stretch when the camera rate collapses under high duty cycle (that is the limit,
  not RAM). Measured live at `run=5`/`gap=2`: window **4,48–4,55 s**, gap **4,29 s**, cycle ~8,8 s.
- **`results[]` is in MEASUREMENT order** (`.index` = combination id, `.block` stamped), so the
  prefix is always complete: aborts need no compaction, and **`GET /results.csv?all=1`** streams
  every measured item live mid-session. ⚠ RAM only — pull it periodically over a long pass; a
  master reboot loses unrepeatable measurements. **Bare `/results.csv` is the 15-row summary,
  NOT the record.**
- **Blocks are the statistics unit**: every `cal_interval_ms` (default **15 min**, `?calint=`,
  0 = no mid-pass insertions) the pass parks for **sweep + baseline together**; the boundary
  closes a block → `/loops` row, drift point, pairwise fold, **and the block centring below**.
- Session wall time scales with `run`/`gap` and combination count. Pause stops the clock, Abort
  publishes the measured prefix.

### Stored z is RAW; RANKING is block-centred (2026-08-13)
Two different values, and the distinction is the whole point:

- **`z_score` is the RAW combined Stouffer z** (Σz_i/√k) and is **never rewritten**. It is the
  archive. `/results.csv` carries it forever, alongside the per-node `z0..z3`.
- **`z_ctr` is what every statistic and every ranking runs on.** At each block close
  `center_block()` subtracts each node's own mean over that block and recombines over the same
  nodes (`have_mask`, so k and the √k scaling are unchanged). Pass mean/σ/χ², Top-N, Bottom-N,
  nearest-zero and the Bonferroni line all read it through the single accessor `rank_z()`.
  Until a block closes, `z_ctr` carries the provisional raw value.

**Why**: the 08-13 full pass had per-node σ ≈ 1,0 *inside* every block while the block offsets
jumped — master to −6,33, slave1 to +24,13 as block means. The noise was fine; the zero point
moved. Re-centred, that pass is a clean null: max |z| **3,92** against the **4,13** expected as
the maximum of 5005 normal draws.

⚠ **Centring also removes any real effect that is CONSTANT across a whole block.** That is a
pre-registration decision, not a detail: what this instrument can still see is an effect that
varies **between items inside a block**. It also shrinks σ by (1 − 1/n_block) — ~0,5 % at ~100
items per block — so σ(z_ctr) sits slightly under 1 by construction, not from degradation.

⚠ **Never pool v3 data with any v2.x session**, and never pool pre- with post-centring passes
without recomputing both the same way.

- UI: **three tables of five** — Top-5 and Bottom-5 and **Nearest-zero-5** (`top[]`/`low[]`/
  `near[]`), plus significance line, item counter + block badge, Save CSV.
  ⚠ **Nearest zero means nearest the PASS MEAN, not nearest raw 0.** `results_near_mean()` picks
  by |z − mean|, which orders identically to the studentized |z − mean|/σ; `/status` publishes
  `pass_mean` and `pass_sigma` so the choice is checkable. The table shows a **Z\*** column.
  CSV is **German**: `;` separator, `,` decimal — a decimal point makes Excel read the whole
  column as text, so the separator alone would not have fixed anything.
- ⚠ The **studentized-view checkbox specified in PLAN.md §2.3 was never built**. The studentized
  value exists only as the `Z*` column. Do not cite it as a feature.
- Firmware is delivered **over OTA only** in normal workflow (`POST /update`); `build.ps1`
  documents build, not serial flash.

### Pass-level null gates and node health
Under H₀ with a working instrument: mean ≈ 0, σ ≈ 1, Σz² ≈ n. **Ranking is secondary; when
`null_flags` ≠ 0 the extremes are not decisive.** Bits: `SIGMA` 0x01, `DRIFT` 0x02, `CHI2` 0x04,
`PAIR` 0x08, published in `/status`.

**Soft-down** (`nodes[].soft_down`) excludes a node from the combine after a block in which its
σ > `NODE_SIGMA_SOFT` **or** |mean| > `NODE_MEAN_SOFT`. Sticky; cleared only after
`NODE_SOFT_CLEAR_BLOCKS` consecutive clean blocks. It never reboots anything.

⚠ **`NODE_SOFT_MIN_COMBINE` is 1, not 3 (2026-08-13).** At four nodes a floor of 3 allowed
exactly ONE exclusion, so when two arms misbehaved the second stayed in. The 08-13 pass is the
proof: slave1 was excluded, the master (block means to −6,33) was kept, and block 4 published
(−6,33 + 0,27 + 0,22)/√3 = −3,38 with the master's offset intact. A bad arm costs more than a
small k (user decision). Up to three of four may now drop out, i.e. a solo combine is possible;
`k` is in the CSV per item so it is visible afterwards.

**Quarantine**: a block whose data a tripping node could have contaminated is excluded from
ranking (`skip_rank=1`) while staying in the CSV. It fires on **every** trip, but **only when
that node was actually in the block's combine** — `center_block()` sets the contribution mask on
the pass it already makes. Both simpler rules are wrong and both were measured:
- *only the first trip* waved through every later bad block (08-13: blocks 4, 14, 16 ranked);
- *every trip unconditionally* excluded 33 of 33 items per block, three blocks running, pass σ
  0,000 — nothing left to rank at all.

## Project structure
- main/elotto.c   – app_main, Ethernet, webserver, HTML/JS UI. Endpoints: `/` `/status` `/start`
  `/abort` `/loops` (per-**block** health) `/results.csv` `/focus` `/pause` `/calibrate` `/pool`
  `/ready` `/probe` `/expose`, plus **`/diag` (a four-camera health PAGE, live, one row per
  node)** and `/diagjson` (the master's own stats). Slaves serve that same JSON at their `/diag`.
  **`POST /expose?exp=<lines>[&gain=<g>]` sets one node's operating point by hand** — served by
  *every* node for its own camera (shared `camera_expose_handle()`), driven from `/diag`'s
  per-row **−/+** buttons. It resets the camera statistics, so `mean_px` answers in ~2 s — the
  point is tuning the physical LIGHT against a live reading. Refused **409** while measuring, and
  the reply carries the **read-back** setting, never the requested one. ⚠ **Not sticky**: the next
  calibration sweep overwrites it, which is correct — the sweep chooses a rung on evidence.
  Omitting `gain` keeps the gain in force.
  ⚠ `cfg.max_uri_handlers` must exceed **(count here) + 5 from elotto_ota**; registration past the
  cap fails and the return value is checked nowhere, so an endpoint just 404s silently.
  Currently 15 + 5 = 20 against a cap of 24.
- main/sensor.c   – GCP analysis, scoring/pooling, baseline, the single pass, blocks, centring,
  drift, soft-down, publishing
- main/nodes.c    – **the array**: UDP link, discovery, calibration handshake, per-node health,
  the drop/reboot policy. `main/nodes.h` is the API; sensor.c reaches other boards only through it.
- main/focus.c    – focus panel, pause, the run gap, and the session clock. These four share
  state, which is why they are one file: `pause_gate()` accumulates the time held,
  `elapsed_ms_now()` subtracts it, and a pause also nudges the gap timer.
- main/sensor.h   – types and declarations
- partitions.csv  – **shared** partition table (factory 1 MB + ota_0/ota_1 3 MB on 32 MB flash).
  Referenced by all three projects; a board flashed by one must be updatable by the others.
- ota_firmware/   – the network updater ("OTA-Firmware"), its own IDF project. Ethernet + HTTP
  + esp_ota only; no camera, no GCP.
- components/elotto_camera/ – OV5647 dark-frame entropy (camera.c, include/camera.h, Kconfig).
- components/elotto_link/ – the UDP wire format between master and slaves
  (`EL1 <seq> <payload>`, ports 5000/5001). One definition compiled into both ends.
- components/elotto_gcp/ – **the z-score primitive itself** (`gcp_zscore_raw()`). The combine is
  Sum(z)/√k over nodes, which is only meaningful if every node computes z identically. A
  divergence would have been invisible: a wrong-but-plausible z looks exactly like a result. The
  two call sites differ only in abort handling, via the `on_yield` callback.
  ⚠ `GCP_SEGMENT_SD` is the literal `7.07106781`, **not** `sqrt(50.0)` — every z the rig has
  recorded came from that constant, and a change in the numbers must never be a side effect of
  tidying.
- components/elotto_ota/ – update endpoint + boot-safety logic (rollback, boot counter,
  mark-valid, /update /boot /reboot /poison /otainfo). `BOOT_FAIL_LIMIT` 3,
  `HEALTHY_UPTIME_MS` 30000.
  All three components are **shared**: the slave repo pulls them via
  `EXTRA_COMPONENT_DIRS=../elotto/components` and ota_firmware pulls *only* elotto_ota by pointing
  at that single component directory — IDF compiles every component it discovers, so pointing at
  `components/` would drag the camera into the recovery image. The repos must stay siblings on
  disk; build, flash and commit them together.

## Nodes
| node | IP | MAC | COM | flash contents |
|------|----|-----|-----|----------------|
| master | 192.168.178.100 | 80:f1:b2:d2:e3:1d | COM4 | factory = updater, ota_0/ota_1 = elotto app |
| slave0 | 192.168.178.103 (static lease) | 80:f1:b2:d2:e3:e5 | — | factory = updater, ota_0 = slave app |
| slave1 | 192.168.178.145 (static lease) | e8:f6:0a:e0:ce:a8 | — | factory = updater, ota_0/ota_1 = slave app |
| slave2 | 192.168.178.155 | e8:f6:0a:e0:c7:a1 | — | factory = updater, ota_0 = slave app |

**Say master/slave0/slave1/slave2, never the IP ending.** ⚠ The **column order in a results CSV
is DISCOVERY order, not the slave number**, and it changes between sessions. The IP list in the
CSV header is what makes `z0..z3` decodable — never map by column position.

**COM ports are not stable** — the same slave has enumerated as COM6, COM8 and COM9. Always list
the ports before an `erase-flash`; a wrong port wipes a working node. Node addresses are
informational: the master finds slaves by UDP broadcast, so a dynamic lease works like a static one.

### Illumination — standing rules
The enclosure is **LIT, not dark**. "Controlled light" was always the goal; it is not the same as
darkness. Sealing it cost raw LSB uniformity and σ — photon shot noise was doing real whitening
work.

⚠ **NEVER power illumination from a node's VSYS pin.** An LED on VSYS with PWM dimming produced
bias −4,33e-3 and certified **0 of 9** rungs. The mechanism is **conducted, not optical** — PWM
current on the rail feeding the sensor's analog supply. It was misdiagnosed as optical flicker
first; the giveaway is that a separate supply restored 8 of 9 rungs *while `mean_px` barely
changed*.

⚠ **Do not judge the light by one `mean_px` reading — sweep, or measure a time series.** A single
number cannot separate a dim lamp from a short exposure, and readings taken during a calibration
sweep are not comparable with settled `/diag` values.

⚠ **After any physical work, let the illumination settle ~30 min before starting a long run**
(2026-08-17). Measured: after the camera swap slave1 read `mean_px` 6,69 at exp 128 during the
sweep and 27,0 settled — a 4× rise that then held to ±0,4 % over 18 min. A sweep run during that
rise picks a rung that no longer applies, which is exactly what moves block offsets. The same day,
a test session started right after the swap put two nodes soft-down while a repeat twenty minutes
later put none.

**Power topology is settled: all four on PoE, permanently (user decision).** The master's separate
USB supply was the Risk-1 control and **will not be restored. Do not re-propose it.** The
consequence, stated once: **inter-node correlation can no longer be attributed to the power rail
versus anything else.** The one measurement that could separate them is already in the bank (an
arm run while the split was intact found master↔slave pairs at +0,023 against slave↔slave +0,024,
largest single pair on the *isolated* node) and cannot be repeated on this rig.

⚠ **Do not switch camera hardware** on the theory that the OV5647 is the problem. The capture runs
at **RAW8 800×800**, ~13 % of the sensor, because the pipeline is PSRAM-bound at a 640 KB diff per
frame pair; more megapixels would *lower* the bit rate. If it is ever tried, buy **one** and run a
matched pair, not four.

## Concept
Four-node ESP32-P4 array. The master scores lottery numbers via GCP methodology; up to three
slaves measure the same window in parallel, triggered by **one UDP broadcast** (port 5000). Slaves
are discovered by broadcast at every session start — no IP table, no node count configured.
Combined z = **Sum(z_node) / sqrt(k)** over the k nodes that answered *that* run, so a node missing
one reply costs that run's gain, not the session.

**The ×√n gain is NOT established** — it assumes the nodes are independent. **Judge a session on
per-block combined σ AND the full pairwise matrix, never on `pair_r` alone**: a pooled worst pair
stayed silent through a growing correlation that σ caught. **σ, not correlation, is where this
array fails** — the 08-13 pass had σ 1,378 with every pairwise |r| ≤ 0,024.

UDP loss is handled explicitly — every frame carries the sequence number it answers, mismatches are
dropped and counted (`net_stale`), and a timed-out command is resent under the same sequence so a
node replies from a one-entry cache instead of measuring twice. **All receive timeouts go through
`link_arm_timeout()`**: lwIP rounds `SO_RCVTIMEO` to whole ms and treats 0 as *wait forever*, so a
sub-millisecond remainder would hang the session — this already happened once.

**Noise source — PHOTONS ONLY (user decision)**:
- **The on-chip TRNG is REMOVED from both firmwares.** Not deselected — deleted. The claim under
  test is about a *physical* random source, and a whitened hardware RNG is an opaque digital
  post-process that would be indistinguishable from the real thing in every statistic this project
  computes. **Do not reintroduce it in any form.**
- Administrative randomness (the Fisher–Yates measurement order) uses an **xorshift32 PRNG seeded
  from the camera** once per session. It never enters a z-score.
- Each node has its **own** OV5647 (never shared — sharing would break independence by
  construction). Entropy = non-overlapping frame pairs, diff = f[2k+1]−f[2k] per pixel (cancels FPN
  exactly), LSB packed, XOR-folded. ~3 Mbit/s per node.
- One source, **session segment count**: `g_status.run_segments` from `?run=` for baseline,
  scoring AND the measurement pass, all behind `g_status.gap_ms`. z stays N(0,1) at any length,
  being normalised by √segments.
- ✅ **The segment count travels on the wire** (`M<seg>`, `B<runs>,<seg>`), so the constant lives in
  `main/sensor.c` only. A slave that is *told* the length cannot disagree. `slave.c` keeps
  `CAM_SEGMENTS` **only** as the fallback for an old master, and logs loudly when it uses it. The
  yield/abort-poll cadence is `nseg/4` on both sides: per-run wall time is max over nodes, so a
  mismatch slows every measurement to the slowest device.
- **A node whose camera stalls is REPORTED, DROPPED and REBOOTED.** There is nothing to fall back
  to by design. The node replies `E:<reason>`; the master names it in `g_status.fault`, drops it
  from the combine, bumps `nodes[].reboots` and sends `R`. The node rejoins the *next* session by
  discovery, never the running one. The session only ABORTS (`src_stalled`) if the drop would leave
  fewer than two nodes.
  ⚠ **The master does not reboot itself** on its own camera failure — that would destroy the
  `/loops` history and the results the operator needs. It faults, reports and aborts.
- A run that dies part-way produces **no z at all**: `gcp_zscore_raw()` returns false rather than a
  short run, because a short run's z would be normalised by a √segments it never reached. Such an
  item is archived with `k=0` (VOID) and never enters ranking.
- **PSRAM is mandatory** with the camera (capture buffers + extraction ring). It also holds
  `g_status.loop_hist` and the per-item **per-node z archive** `s_node_z` (`NUM_RUNS × MAX_NODES`
  floats, ~128 KB) that block centring and post-hoc recombination read. Internal RAM is full with
  `results[]`; adding a few KB of .bss fails the *link*, not the run.
- **Task priority is load-bearing**: the extraction task (`ELOTTO_CAM_TASK_PRIO` = 4) is
  CPU-hungry, so any task calling `camera_read_word()` must run *above* it or the producer starves
  the consumer (~10× slowdown; signature is ring `drops` huge with `waits == 0`).

Phase 1: baseline (all nodes in parallel), repeated at **every block insertion**. **The
subtraction is GONE, not just inert.** The baseline is purely the **drift reference**:
`LoopStat.base` per block, the independent cross-check against the block's own master mean
(`raw_m`, which is what `drift_add()` regresses on). **Default 10 runs** (`?baseline=`). The UI bar
is labelled **"Baseline — drift reference"**, not "Calibration": the camera exposure sweep is a
different phase, and having two things called calibration in one interface was ambiguous. The
`cal*` element ids are historical.

**Two attended gates, both opt-in on `POST /start?confirm=1`.** The web UI always sends it; curl
never does.
- **`PHASE_READY` — the observer gate.** After calibration and baseline the session parks and shows
  a big **Start** button; `POST /ready` releases it. Scoring is the first phase whose bits are
  collected while a target is on screen, so it is where the protocol actually begins. **No
  timeout**, deliberately: there is no sensible default action. Releasing it holds **1 s dark**
  (`READY_SETTLE_MS`) before the first number — pressing it is itself an act of attention, and
  onset is the payload. The phase moves to `PHASE_SCORING` *before* that delay, or a `/status` poll
  still seeing `ready` would re-raise the overlay.
  ⚠ **Armed on `focus_mode`, not merely on `confirm` (2026-08-13).** No observer, no observer gate.
  The UI sends `confirm=1` unconditionally, and with no timeout that parked a 5005-item unattended
  run behind a Start button nobody was there to press: the 08-13 pass ran **37,9 h wall against
  12,2 h of measuring**, i.e. ~25,7 h stalled at a gate.
- **`PHASE_POOL_CONFIRM` — pool confirmation.** After scoring, the proposed pool is published and
  the operator can edit it: `POST /pool?act=ok|more|cancel&main=..&euro=..`. "Select more"
  re-scores with the still-checked numbers **omitted** from the pass, so they keep the measurement
  that chose them. Keeping exactly 5+2 gives **ONE** combination, which is intended and is the
  highest-power way to use the instrument. ⚠ **15-minute timeout** accepts the proposal unchanged
  and records `pool_auto=1`. When `focus=0` it accepts **immediately** and records the same flag,
  rather than burning the timeout on an operator who is not there.

Phase 0: score individual numbers 1..N with **exactly one node-combined run each, but a LONG one**
— the session window, default 5 s — sweeping the numbers in a **fresh random order (Fisher–Yates,
no repeats)**. Selection direction is pre-registered: `?score=high|low|abs`, default `high`.
⚠ Under H₀ the combined z is already N(0,1): its SE is **1,0**, not 1/√k. Ranking 50 unit-noise
draws is therefore very noisy — a real cost. It changes only *which* numbers enter the pool, never
the Phase-2 statistics measured on them.
⚠ **Never repeat a target in place**, in any form. The phase has been through four shapes and two
were rejected on the same ground: repeated short reps froze the panel and only the first window had
an onset. One long run gives the same arithmetic — a 3× run is Σdev/√(3N), i.e. the Stouffer
combination of three 1× runs — while keeping **one onset per number**, which is the payload.
⚠ **The gap scales with the window** — `?gap=`, default 40 % of `?run=`. Duty cycle, not window
length, is what starves the extraction task (cliff past ~75–80 %).
⚠ `LINK_MEAS_MS` is gone — the reply window is `LINK_MEAS_MS_FOR(nseg)`. A flat timeout was
headroom for a short run and a **deadline** for a long one: every slave would still be measuring
when it expired, all would look silent, and after `NODE_MISS_LIMIT` they would be DROPPED, leaving
a solo session that still looked healthy. Deliberately generous: a late drop costs nothing, a false
drop costs an arm of data measured at √(k−1) unnoticed.

Phase 2: **ONE pass over the whole combination space** in one Fisher–Yates random order (drift
immunity), each item measured exactly once at the **same session window as every other phase**.
Node independence: `PairAcc[i][j]` accumulates per-**block**-centered moments for EVERY node pair
(6 at n=4), only over runs where both contributed → the full Pearson matrix plus per-node σ in
`/status` (⚠ if |r|·√n > 3).
Cross-block drift: `close_block()` → `record_loop()` stores per-block raw offsets/σ per node +
camera health (LoopStat, PSRAM, first LOOP_HIST=128 blocks, served by **/loops**), and
`drift_add()` regresses the master's per-block mean on the block index → `drift_slope` / `drift_t`;
|t| > 3 flags real drift. The slave's per-block camera numbers come from the `D` command, queried at
block close while it is idle — a missing reply is diagnostics-only.

**Focus display**: a "Focus:" card shows the current target in large type for exactly the window its
bits are collected in — the candidate number while scoring, the whole draw while measuring — so the
observer is present *while* the noise is sampled (the original GCP/PEAR protocol). It changes
nothing statistically, so a session is merely **tagged**: `/start?focus=1`, `"focus"` in /status and
`# focus=on|off` in the CSV. **Attended and unattended sessions must never be pooled.**
- `GET /focus` (~60 B) is polled at **10 Hz** and is deliberately separate from the 2.5 KB /status:
  `seq` is monotonic per window, so the UI counts *missed* windows (a skipped window credits an
  effect to the wrong combination — mislabeling, not blur). `POST /pause?on=1|0` holds **between**
  runs only; state stays `running`, and paused time is excluded from `elapsed_ms`.
- Attended sessions flush the camera ring briefly before each window (`attended_onset_settle()`,
  500 ms cap) so pre-onset bits do not enter the run the observer is about to watch. Master-only:
  slaves have no settle command on the wire.
- ⚠ `camera_get_stats()` is cumulative **since the last `camera_stats_reset()`**, i.e. since the
  last sweep. In a `?cal=0` session there is no reset and they are lifetime averages — a falling
  `mbit_s` is then not evidence of live degradation.

**Camera calibration (per BLOCK)**: at every insertion the master broadcasts `K<budget_ms>`, sweeps
its own exposure ladder in parallel, and waits for every node's
`OK:<exp>,<gain>,<fold>,<bias>,<mbit_s>,<G|U>`. Each node keeps the setting with the **lowest
|bias − 0.5| among candidates clearing the σ gate with margin** (|σ−1| ≤ half the tolerance),
falling back to the bare gate if none qualify. The margin exists because a bias-only rule put a node
on a rung it could not sustain in 91 of 127 loops. The budget is a *cap*, not a target, so progress
must never be estimated against it.
⚠ **The default cap is 10 s.** 5 s was too short — a long pass left all nodes with `cam_cal=0`. The
cap is divided evenly over the 9 candidates, so each rung is scored on ~1,1 s of bits; per-rung
bias/σ are correspondingly noisy, and rungs near a gate boundary flip between sweeps. That is the
failure the σ-margin rule exists to contain. `?cal=<ms>` restores any budget without a reflash;
`POST /start?cal=0` turns it off, and per-loop calibration is **statistically neutral** (A−B = 0,52
SE over 6×430 runs per arm).
`GET /calibrate` serves the whole last sweep per candidate with the gate each failed — **on every
node**, which is what makes a per-node optical fault diagnosable at all.
**Nodes land on different exposures on purpose** (different sensors, different light); what they
must share is the segment count, which travels on the wire.
The chosen setting is recorded per block in `/loops` — mandatory, because a re-tune nobody logged is
indistinguishable from drift in the data.
⚠ `CAL_MAX_MEAN_PX` is **100.0**. It is a light-*leak* floor written when the cameras were meant to
sit dark; the enclosure is deliberately lit, so a high `mean_px` means the lamp is on.
⚠ **The trigger is TIME.** Default **15 min**, `POST /start?calint=<ms>`; **0 = no mid-pass
insertions**. Thermal drift moves on wall-clock time, and the insertion cadence also sets the drift
regression's resolution and the block size the centring estimates its means from.

Modes: Eurojackpot (5 of 50 + 2 of 12, 7920 combinations) and 6 of 49 (5005 combinations).
⚠ **`?mode=` is `1` for 6-of-49 and ANYTHING ELSE for Eurojackpot** — it tests `val[0]=='1'`, so
`?mode=649` silently starts a Eurojackpot session.

## Build, Flash, Monitor

**Build** — always through `build.ps1`, which sets the environment and forwards its arguments.
Shell state does not survive between tool calls, so bare `idf.py` needs the env re-exported every
time; and the script must use the **VS Code extension's** venv
(`C:\Espressif\tools\python\v6.0.1\venv`), not `export.ps1`'s, or the build dir gets pinned to the
wrong interpreter and fails with "run 'idf.py fullclean'".

```powershell
.\build.ps1 build                        # master
.\build.ps1 -C ota_firmware build        # updater  (-C selects another project)
.\build.ps1 -C ../elotto_slave build     # slave
```

**Flash — over Ethernet, not USB.** Firmware is pushed to the running node:

```powershell
curl http://192.168.178.100/update --data-binary @build/elotto.bin                        # master
curl http://192.168.178.103/update --data-binary @../elotto_slave/build/elotto_slave.bin  # slave
```

~750 KB in ~3 s. The node writes the *inactive* slot, reboots, and marks itself valid only once its
webserver answers, so a failed transfer or a dead image cannot strand it. `POST /update?slot=0|1`
targets a slot explicitly; `POST /boot?slot=N` selects the next boot slot and clears the fail
counter — that is how a node is put back on a known-good image without USB.

`/update` and `POST /start` return **409** while a session runs. Abort first, then start.

⚠ **A node that pings but refuses port 80 is not dead** — it is an image that never reached
`mark_valid()`, or a board sitting in its factory updater. Check `/otainfo` (updater) or
`/info` (older updater builds) before reaching for USB. `webserver_task` waits **30 s** for an IP
and then deletes itself without retrying, so a slow DHCP lease leaves exactly that state and the
image is rolled back on the next boot. This took all four nodes down once (2026-08-13) and looked
like four simultaneous firmware failures.

**USB is only for:** a fresh board (bootloader + partition table + factory updater), or a node whose
recovery updater is gone. OTA cannot repair those.
`.\build.ps1 -C ota_firmware -p COMx erase-flash` then `... -p COMx flash`.

| Action                  | VS Code shortcut |
|-------------------------|------------------|
| Build only              | Ctrl+Shift+B     |
| Menuconfig              | Ctrl+E G         |

## Rules
- Never edit sdkconfig manually. To change a default, edit `sdkconfig.defaults`, delete
  `sdkconfig`, and let the build regenerate it (verify the diff afterwards). Every project has a
  `sdkconfig.defaults` and sets `IDF_TARGET` in its CMakeLists — without both, a regenerate loses
  settings or fails with "CMAKE_C_COMPILER not set".
- Target is always esp32p4
- The slave's `sdkconfig.defaults` must keep `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` **before**
  `ESP32P4_REV_MIN_0=y` — the latter depends on it, and without it the choice silently falls back to
  rev v3.1 and the binary refuses to boot on these v1.3 boards.

## Where things stand (2026-08-17) — read this first

**The instrument is sound and every result so far is null**, which is what a working null instrument
produces. `main/` is split four ways (elotto/sensor/nodes/focus) and the z-score primitive is shared
with the slave via `components/elotto_gcp`.

**Hardware, current:** all four nodes lit and healthy at idle — bias within 3,1e-4 of 0,5, σ 0,999
to 1,002, autocorr ≈ 0, 3,4 Mbit/s each. **slave1's and slave2's cameras were swapped on
2026-08-17** to test whether slave1's fault follows the sensor or stays with the board; that
question is **open and needs a loaded run to answer**, since neither node shows anything wrong at
idle. Settled light at exp 128: master 34,5 · slave0 27,9 · slave1 27,0 · slave2 19,5, stable to
±0,6 % over 18 min.

**`docs/data/` holds the post-2026-07-29 sessions.** Nothing recorded before that hardware change
may be pooled with anything after it.

| directory | what |
|---|---|
| `2026-08-05_6of49_fullpass/` | 5005 items, ~14 h. ⚠ ran with `cam_cal=0` on all four nodes |
| `2026-08-08_6of49_aborted3404/` | 3404 items + all four ladders |
| `2026-08-08_6of49_pool11/` | 462 items, complete, first pass on the results code |
| `2026-08-13_6of49_fullpass_unattended/` | 5005 items, uncentred, with the README that motivated centring |
| `_live_*` | a second complete 5005 pass, 13,4 h, started via curl so no gates |
| `_short_*` | a 1995/5005 partial, 57 blocks at `calint` 5 min |
| `_analyze_*.py` | the operator's own analysis scripts |

**Open, in the order I would pick them up:**
1. **Does slave1's fault follow the camera?** The swap is done; only a loaded run answers it. Both
   nodes are clean at idle, and slave1's block-mean excursions (+24,13 in one block) never showed
   up in an idle reading.
2. **The master is the other bad arm** — 9 of 49 blocks with |mean| > 1,5, worst −6,33. It has never
   been diagnosed separately from slave1 because the old soft-down floor could only ever exclude
   one of them.
3. **Verify the centring on a full pass.** It is verified on a short run (closed blocks at
   mean(z_ctr) exactly 0,0000, open block provisional) but has never run a full session.
4. **Does calibration reduce the RATE of bad blocks?** The control pair only ever asked "does it add
   variance?" (no). The tail question needs a count of excursions over many blocks, not a mean.

**Deferred by the user — do not start unasked:** the attended-vs-unattended (focus) comparison.
**Dropped by decision — do not re-propose:** node-drop test, camera-fault/reboot path, camera-stall
abort, restoring the master to USB power.

⚠ Superseded design notes, the pre-2026-07-29 optics measurements and the v2 loop/ranking era were
removed from this file and from `docs/PLAN.md` on 2026-08-17. They remain in git history at commit
`144ed5e`, e.g. `git show 144ed5e:CLAUDE.md` and `git show 144ed5e:docs/PLAN.md`.
`docs/PLAN_4NODE.md` and `docs/PLAN_NETWORK.md` were deleted earlier and are at `8e134e5`. Source
comments still cite those two by name; the citations are historical and resolve to git history.
