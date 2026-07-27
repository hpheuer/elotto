# PLAN: elotto

Status: **new (2026-07-26)**. This replaces `PLAN_4NODE.md` and `PLAN_NETWORK.md`, which were
deleted at the user's request. They are not lost — both are in git history, last present at
commit **`8e134e5`** (`git show 8e134e5:docs/PLAN_4NODE.md`), and every gate result they recorded
is summarised in `CLAUDE.md`. Source comments still cite them ("PLAN_4NODE Phase 3",
"PLAN_NETWORK §4"); those citations are **historical** and resolve to git history, not to a file
on disk. They were left alone deliberately — rewriting them to point here would make them cite
phases this document does not contain.

What those plans delivered, in one line each, because Task 1 builds directly on it: OV5647
dark-frame entropy on all four nodes; UDP broadcast trigger with explicit loss handling;
Ethernet OTA; per-loop studentization, Stouffer accumulation and a full pairwise independence
matrix; and a Focus display that holds each target on screen for exactly the window its bits are
collected in.

---

## Task 1 — Camera calibration at the start of every loop

**What is asked.** The master calibrates itself and commands the slaves to calibrate themselves;
it waits for each slave's finishing acknowledgement. Calibration means *adjusting the camera
parameters for the maximum / best random data stream*. It runs at the beginning of each loop.

### 1.1 Code review — what already exists

**The command/ack structure is already there, and already per-loop.** This is the good news: the
shape the task describes is the shape the baseline phase already has.

- `sensor.c` → `slave_baseline_start(runs, nseg)` broadcasts `B<runs>,<nseg>` to every node in one
  datagram; the master then runs its *own* baseline locally, in parallel; `slave_baseline_wait()`
  calls `nodes_collect(baseline_total * 800 + 15000, true)` and blocks until every healthy node
  has replied `OK`.
- `slave.c` → on `B` it re-arms its camera, runs the requested runs, replies `OK`.
- Both sit inside `for (int loop = 0; loop < loops_total; loop++)`, i.e. **already at the start of
  every loop**.

So "master does its own, commands the slaves, waits for the ack, once per loop" needs no new
transport. What changes is what the nodes *do* when commanded.

**⚠ But today's "calibration" is not camera calibration at all.** The baseline phase measures the
per-run z offset (`baseline_mean`) and subtracts it from later runs. It reads no camera metric
and writes no camera register. The name is the only thing the two have in common.

**The camera parameters are runtime-writable, and the plumbing is already proven.**

| what | where | range | current |
|---|---|---|---|
| exposure | regs `0x3500`–`0x3502` | 1 … 1048575 | **16** |
| analog gain | regs `0x350a`/`0x350b` | 0 … 1023 | **1023 — already maximum** |
| AEC/AGC manual | reg `0x3503` = `0x03` | — | set at init |

`cam_reg_write()` / `cam_reg_read()` in `camera.c` go through the V4L2 `S_REG`/`G_REG` ioctl and
are already used after `camera_init()` and again after `STREAMON`. `cam_verify_regs()` reads the
registers back, because **writing them is not proof they stuck** — the driver rewrites the format
array on `S_FMT`, and some OV5647 revisions latch exposure only via group-hold. Any calibration
loop must keep that read-back or a silently ignored setting will look exactly like a working one.

Gain is already pinned at maximum, so **exposure is the knob that actually has room**.

### 1.2 Two blockers that must be built before any sweep can work

**(a) `camera_get_stats()` is cumulative since stream start — it cannot score a candidate
setting.** `s_bits_extracted`, `s_ones_count`, `s_autocorr_*`, `s_pixel_sum`, `s_zero_diffs` and
the `s_z_*` sigma accumulators are never reset, and `mbit_per_sec` divides by
`now − s_stream_start_us`. Measure setting A, switch to B, measure again, and the second number is
mostly still A. This is the same trap that made a falling `mbit_s` look like live degradation
earlier. **Needs `camera_stats_reset()`, or a windowed snapshot API, before anything else in this
task is possible.** It is the first thing to build.

**(b) After changing exposure the pipeline is full of stale state.** Frames already in the driver
queue were captured under the old setting, and the ring buffer holds words extracted from them.
Both must be discarded and the sensor given a few frames to settle before the new setting is
measured, or every score is a blend of two settings. Needs a defined settle-and-flush step —
discard N frame pairs *and* drain the ring — with N chosen from the measured frame interval.

### 1.3 The objective must be decided — "maximum" and "best" pull against each other

This is the one genuine design question in the task, and it cannot be left implicit.

More exposure → more photons → more shot noise → a wider diff distribution → fewer zero-diffs →
a more uniform LSB → **better bias**. That relationship is measured, not assumed: the two cameras
differed at identical settings, and bias tracked light level.

| | mean_px | zero_diff | bias deviation |
|---|---|---|---|
| master | 6.80 | 9.4 % | 3.7e-5 |
| slave | 2.84 | 16.2 % | 8.6e-4 |

But more exposure → longer frame time → fewer frames per second → **lower Mbit/s**. So "maximum"
(rate) and "best" (quality) are in direct tension and a single number cannot maximise both.

**DECIDED (user, 2026-07-26): best entropy, not maximum rate.** Among the settings that pass the
quality gates, pick the one with the highest sustained Mbit/s — rate is the tie-break only, never
a reason to accept a measurably less uniform stream.

- Hard gates (inherited from the original Phase 0 gate, which these cameras have met before):
  `|bias − 0.5| < 1e-3`, `|autocorr lag 1..4| < 0.01`, per-mini-run σ within 1 ± 0.05,
  zero stuck frames, and `mean_pixel_level` below the light-leak threshold.
- Tie-break and only tie-break on rate.

Rationale: the entropy is the instrument. A faster stream that is measurably less uniform buys
granularity at the cost of the thing being measured, and the project has consistently refused
that trade (a stalled camera drops the node rather than substituting the TRNG).

**The XOR fold is in scope** — it is a *processing* parameter, and the task explicitly covers
"camera or processing parameter". `CONFIG_ELOTTO_CAM_XOR_FOLD` halves the bit rate to square away
the raw LSB bias, so a setting whose *raw* bias already passes is worth twice the stream — a
larger win than the exposure sweep alone will produce. It is now a runtime flag
(`camera_set_xor_fold()`), Kconfig setting only the power-on default, so the sweep can try it.

### 1.4 Proposed design

**Protocol.** A new command rather than overloading `B`, so calibration and baseline stay
separable and a session can skip one without the other:

```
K<budget_ms>   ->  OK:<exposure>,<gain>,<bias>,<mbit_s>     (calibrate, then report what was chosen)
```

Same framing as everything else (`EL1 <seq> <payload>`), so sequence handling, the resend-under-
the-same-seq rule and the one-entry reply cache all apply unchanged. The reply carries the chosen
setting so the master can log per-node settings without a second round trip.

**Per-node procedure** (identical code on master and slave — it belongs in the shared
`elotto_camera` component, not duplicated, for the same reason the extraction pipeline is shared):

1. For each candidate exposure on a coarse ladder:
   apply → verify by read-back → settle and flush → `camera_stats_reset()` → collect a fixed
   number of bits → score.
2. Keep the best scoring setting that passes the gates; apply it; verify by read-back.
3. Report it.

**Each node calibrates its own camera and they will land on different settings — that is correct,
not a bug.** The cameras are physically different units; that is precisely why one was cleaner
than the other at identical settings. Nothing requires the nodes to share a setting. What they
*must* still share is the segment count per run, which already travels on the wire.

**Master waits for all acks** with a timeout derived from the budget, exactly as
`slave_baseline_wait()` does today. A node that fails to answer is handled by the existing rule —
dropped after `NODE_MISS_LIMIT`, session continues over √(k−1).

### 1.5 Consequences that need deciding, not discovering later

1. **Per-loop cost — DECIDED: full sweep every loop.** A sweep of K settings × T seconds runs at
   the start of *every* loop, and a loop is ~10 min; ten candidates at 3 s each is ~30 s, about
   5 % overhead. The reason for the full sweep rather than a cheap re-verify is physical: the best
   setting is not assumed constant. Temperature drifts over a session, and the achievable window
   was already measured moving 1.75× across a day at fixed settings — so a sweep that re-derives
   the optimum each loop is the point, not overhead to be optimised away. Keep the ladder coarse
   enough that the cost stays near 5 %.
2. **Different sensor settings per loop are ACCEPTED (user, 2026-07-26).** Loop 1 and loop 2 may
   be measured at different exposures. This is deliberate: re-deriving the optimum each loop is
   what tracks thermal drift. It is safe for the statistics because `studentize()` removes each
   loop's own offset exactly, the same mechanism that already absorbs the camera's residual bias.
   It is **not** the same as mixing entropy sources mid-session — the source stays the camera
   throughout; only its operating point moves.
   Still mandatory: **record the chosen settings per loop** in `LoopStat` and `/loops`, so any
   effect stays attributable to the setting that produced it. Do not ship the re-tune without the
   record — a per-loop change nobody logged is indistinguishable from drift in the data.
3. **Exposure changes the bit rate, and the rate sets the run window.** Per-run wall time is the
   max over nodes, so a slave that calibrates to a slower setting slows every run for everyone.
   And the window is already unstable (±35 % across a day at a fixed segment count), so
   calibration will move it further. `focus_win_ms` must be watched across a calibrated session.
4. **Interaction with the Focus display.** Calibration happens at the start of a loop, before the
   measurement phase, so the panel should stay hidden throughout it — as it already is during
   baseline. Nothing is being attended to while the sensor is being tuned.
5. **Pause during calibration.** `pause_gate()` is deliberately not called in the baseline loop,
   because one `B` sets every slave running autonomously and a master-side hold would
   desynchronise them. The same applies to `K`. Calibration is not pausable.

### 1.6 Gate

- `camera_stats_reset()` exists and is proven: two consecutive windows at the *same* setting agree
  within sampling error, and a window after a setting change contains no contribution from the
  previous setting.
- A full sweep on one node produces a monotonic, explainable bias-vs-exposure curve rather than
  noise — if bias does not respond to exposure, the premise is wrong and the task stops there.
- The chosen setting passes every hard gate, verified by read-back after it is applied.
- All four nodes calibrate in parallel from one broadcast, every node acknowledges, and the master
  logs the per-node chosen settings.
- A node that does not answer is dropped by the existing rule; the session continues.
- Per-loop calibration cost is measured and stays inside the agreed budget.
- Chosen settings appear per loop in `/loops`.
- `loop_sigma` ≈ 1 and the pairwise matrix stays clean on a calibrated multi-loop session — the
  retune must not disturb the statistics, which is the one way this could do real damage.

### 1.7 Decisions (settled 2026-07-26)

1. **Objective: best entropy, not maximum rate.** Quality gates first, rate as tie-break only.
2. **Full sweep at the start of every loop.** Camera *and* processing parameters may need
   re-deriving; temperature drift is expected.
3. **The XOR fold is in scope** as a processing parameter.
4. **Different settings per loop are fine**, provided they are recorded per loop.

### 1.8 Implementation status

**Done — the foundation from §1.2, in the shared `elotto_camera` component (both ends get it):**

- `camera_stats_reset(settle_pairs)` / `camera_stats_settled()` — discards `settle_pairs` frame
  pairs, empties the ring, then zeroes every entropy accumulator and restarts the rate clock, so
  `camera_get_stats()` describes only the window that starts now. Performed by the capture task
  itself, so it cannot race the producer. Health counters (`ring_drops`, `consumer_waits`,
  `stalls`) are deliberately left cumulative — other code reads them as lifetime totals.
  This also fixes the trap noted in CLAUDE.md: with a reset per loop, `mbit_s` and `bias` become
  per-window figures instead of misleading lifetime averages.
- `camera_set_exposure(exposure, gain)` — applies and **verifies by read-back**, returning false
  if the sensor did not latch it. Without this a silently ignored write would make the sweep
  score the previous setting and then "choose" it.
- `camera_get_exposure()`.
- `camera_set_xor_fold()` / `camera_get_xor_fold()` — the fold became a runtime flag
  (was `#if CONFIG_ELOTTO_CAM_XOR_FOLD` inside the hot loop); Kconfig now only sets the
  power-on default.

**Done (2026-07-26) — steps 1–4 below are all built, flashed to all four nodes and verified on
hardware.** The four decisions in §1.7 are implemented as written.

- `camera_calibrate(budget_ms, abort_cb, camera_cal_t*)` in the shared component. Ladder
  `{4, 8, 16, 32, 64, 128, 256, 512}` exposure lines, preceded by a re-measure of the setting
  already in force and followed by one XOR-fold trial. Gates: `|bias−0.5| < 1e-3`,
  `|autocorr 1..4| < 0.01`, σ within 1 ± 0.05, zero stuck frames, `mean_px < 64`, and at least
  2 Mbit / 200 mini-runs in the window (below that the window cannot support a 1e-3 decision —
  SE(bias) = 0.5/√bits). Tie-break on rate, only among candidates that pass.
  **If nothing passes, the entry setting is restored and `ok` is false** — never "best of a bad
  lot", which would move the operating point to something no gate certified.
- Wire protocol: `K<budget_ms>` → `OK:<exp>,<gain>,<fold>,<bias>,<mbit_s>,<G|U>`. The trailing
  tag is one field beyond what §1.4 specified: it distinguishes a node that adopted a **G**ated
  setting from one that kept its previous one (**U**ncertified. Without it the two look identical
  in `/status`). Same idiom as the `Z:` reply's `,C|T` source tag.
- `calibrate_all()` runs at the top of every loop, before the baseline — the baseline estimates
  the offset of the runs it is subtracted from, so it must be measured at the same operating
  point. Not pausable, same reason as the baseline. Skipped entirely for a TRNG session.
- `LoopStat` + `/loops` carry `cam_exp/gain/fold/cal/bias` per node and `cal_ms` per loop (§1.5.2).
- `/status` carries the same per node; the UI's node table gained an `exp` column
  (`128⊕` = fold on, trailing `!` = uncertified).
- `GET /calibrate` serves the master's whole last sweep table, per candidate, with the gate
  bitmask each one failed. Read-only — the sweep only runs inside a session, so this reports the
  real code path rather than a manual one that could diverge from it.
- `POST /start?cal=<ms>` (default 30000, `0` = off) — the matched no-calibration control, and the
  only way to measure what the sweep costs.

**Two bugs found by the first hardware run, both fixed in `camera.c`:**

1. `camera_stats_reset()` zeroed the producer's accumulators but not the **published snapshot**
   `camera_get_stats()` serves — `publish_stats()` only refreshes it after a pair is extracted.
   Every one of the first sweep's ten candidates was therefore scored on the same stale
   cumulative snapshot: each window passed its bit target the instant it opened, all ten rows came
   back byte-identical, and the sweep "chose" in 2.4 s of a 30 s budget. The reset now zeroes the
   published mirror too, so an unmeasured window reads as empty.
2. The settle countdown cleared `s_settle_pairs` **before** running the reset. Since the caller
   runs above the capture task, it could observe `camera_stats_settled() == true` and read the
   previous setting's numbers as the new window's. Reset now happens first, flag second.

### 1.9 Gate results (2026-07-26, all four nodes)

| gate | result |
|---|---|
| `camera_stats_reset()` proven | ✅ step 0 (exposure 128) vs the ladder's own 128 rung: bias 0.500222 vs 0.499922 — 3.0e-4 apart against SE(diff) 2.5e-4, i.e. 1.2 σ. Agrees within sampling error. |
| monotonic bias-vs-exposure curve | ✅ see below — the premise holds, decisively |
| chosen setting passes every gate, verified by read-back | ✅ `camera_set_exposure()` re-verifies on apply; `ok=true` requires it |
| four nodes calibrate from one broadcast, all ack, settings logged | ✅ master 128, `.103` 512, `.155` 128, `.145` 512 — all `cam_cal=1` |
| per-loop cost inside budget | ✅ 26.8 s and 26.6 s against a 30 s budget = **4.4 %** of a 10 min loop |
| chosen settings in `/loops` | ✅ per node per loop |
| a node that does not answer is dropped | ⬜ not exercised — same long-open node-drop test as CLAUDE.md item 2 |
| `loop_sigma` ≈ 1 and clean pairwise matrix on a calibrated multi-loop session | ⬜ needs a real session; the smoke test ran 2 runs/loop, where σ is undefined |

**The curve** (master, fold on, 8 Mbit per window):

| exposure | bias − 0.5 | zero_diff | mean_px | verdict |
|---|---|---|---|---|
| 4 | **−4.9e-3** | 19.9 % | 2.6 | fail: bias + σ |
| 8 | −1.9e-3 | 15.5 % | 3.3 | fail: bias |
| 16 *(old default)* | −3.8e-4 | 10.8 % | 4.9 | pass |
| 32 | −1.9e-4 | 7.7 % | 8.6 | pass |
| 64 | −9e-6 | 6.3 % | 16.9 | pass |
| 128 | −7.8e-5 | 5.1 % | 33.8 | **pass — chosen** |
| 256 | +2.0e-5 | 3.9 % | 68.6 | fail: light floor |
| 512 | −1.0e-3 | 3.7 % | 140.8 | fail: light floor |
| 128, fold **off** | −1.7e-3 | 5.0 % | 34.1 | fail: bias (but 6.34 Mbit/s) |

Exactly the predicted mechanism: more exposure → fewer zero-diffs → more uniform LSB, until the
frame stops being a dark frame. **This retires the Kconfig default of 16 as an operating point** —
it passed, but sat an order of magnitude worse in bias than 64/128. And the fold-off row settles
§1.3's open question with a number: the raw LSB bias is 1.7e-3 and the fold takes it to 7.8e-5,
so the fold is doing real work and doubling the stream is not available at this exposure.

**Things worth knowing for the next session:**

- **The nodes chose different exposures and that is the design, not a fault.** The master's 512
  saturates at mean_px 141 while two slaves ran 512 inside the light gate — the sensors sit in
  different light. This is the same physical difference that made one camera cleaner than the
  other at identical settings in §1.3.
- **The optimum really does move between loops**, which is the entire justification for §1.5.1:
  `.145` chose 512/fold-off in loop 1 and 512/fold-on in loop 2; `.155` went 256 → 128. Had this
  been a re-verify instead of a full sweep, those changes would have been missed.
- **The `mean_px < 64` light-leak floor is now the binding constraint on the master's ladder** —
  it, not bias, is what stops it going past 128. The threshold is a judgement call (a quarter of
  full scale, chosen so the gate catches a lit room rather than capping the intended increase).
  Every step publishes its measured `mean_px`, so it can be revisited against data.
- **Abort during a sweep is exercised and clean**: `ok=false`, `chosen=-1`, entry exposure
  restored, no node dropped, `net_lost=0`, all three slaves back to streaming.

### 1.10 The 10-loop gate session (2026-07-26) — TASK 1 GATE PASSED

10 loops × 430 runs Eurojackpot, 4 nodes, camera-only, calibration on, display live with a
6.4 Hz `/focus` poller standing in for the browser. 2 h 14 min, 4300 measurement runs.

| loop | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| σ | 1.052 | 1.013 | 1.026 | 0.960 | 0.992 | 0.995 | 1.013 | 0.971 | 1.050 | 1.081 |

**Mean σ = 1.015 ± 0.012** — 1.3 SE from unity. The decisive detail is not the mean but the
spread: SD across loops is 0.038 against the 0.034 expected from sampling alone at 430 runs.
**The per-loop retune adds no variance component of its own**, which was the one way Task 1
could have done real damage.

| gate | result |
|---|---|
| `loop_sigma` ≈ 1 over a calibrated multi-loop session | ✅ 1.015 ± 0.012 |
| pairwise matrix stays clean | ✅ all six pairs \|r\| ≤ 0.0145 over 4300 runs; worst \|r\|√n = **0.95** vs threshold 3 |
| per-loop cost inside budget | ✅ 25.0–25.9 s/loop = **3.2 %** of 134 min |
| chosen settings in `/loops` | ✅ per node per loop, all 10 loops |
| a node that does not answer is dropped | ⬜ still not exercised — no node failed |

**Open item 1 (inter-node correlation growth) DOES NOT REPRODUCE.** The historical session had
σ 1.038 → 1.083 → 1.182 with a pooled worst pair of +0.064. This one is flat at 1.015 with a
worst pair 4× smaller on 10× the runs, same array, same power topology. Not proof the ×√n gain
is established — the mechanism behind the original growth was never identified, so a differing
condition is still possible — but the finding as recorded no longer holds.

**Open item 4 (window drift) DOES NOT REPRODUCE.** Window 1102.0–1115.1 ms across the session
(spread 1.2 %), gap 347.9–348.2 ms against the 350 ms constant, `drift_slope` −0.0054 z/loop at
t = −0.20. Item 4 recorded 1.75× variation across a day and 8.3 % creep over 1700 runs.

**§1.5.3's concern is resolved, with a mechanism.** Exposures ranged from **4 to 512 across
nodes simultaneously** all session and the gap never moved off 348 ms. The bit rate is
**CPU-bound, not exposure-bound**, so differing exposures do not desynchronise the nodes. Only
the XOR fold did, by genuinely doubling one node's rate — which is the second reason it is out
of the sweep.

Other results: 4362 focus windows published, **0 missed**, 0 HTTP errors (4362 = 4300 draws + 62
scoring, i.e. every window observed). No camera faults, reboots, stalls, lost triggers or stale
replies; 4/4 nodes throughout. `best_z` = 3.011 against an expected max |z| of ≈3.0 for 430
draws from N(0,1) — the instrument behaves as a null instrument should.

⚠ **All of the above was measured with the cameras OPEN ON THE BENCH — no enclosure.** The
master's `mean_px` at a fixed exposure of 16 read 4.89 in the morning and 10.34 in the
afternoon: its light level roughly doubled over the day. That is why its chosen exposure walked
down 64 → 4 across the session, and why exposure 4 (bias 0.4951, failing, in the morning sweep)
measured 0.4998 by the evening. **The calibration tracking this is the system working as §1.5.1
intends**, but it means three things are conditional on bench light and must be re-derived once
the cameras are enclosed (user is building one, 2026-07-26):

- the chosen exposures and the §1.9 bias-vs-exposure curve,
- the `mean_px < 64` light-leak threshold,
- the matched `?cal=0` control, which is therefore **deliberately deferred** — running it now
  would compare against conditions the enclosure invalidates.

The σ and independence results are *not* conditional on this: they concern the statistical
behaviour of the combined z, not the absolute light level.

**Fixed after the session:**
- `camera_calibrate()` reported `bias 0.000000` when no candidate passed, which in `/loops` read
  like a catastrophic bias rather than "not certified". It now reports step 0's measurements —
  the setting actually in force — with `ok` carrying the certified/not distinction.
- `drift_t` is published only from **6** loops (`DRIFT_MIN_LOOPS`), not 3. At three loops the
  regression has one degree of freedom, where the 5 % critical value is 12.7; it duly fired
  +10.30 at loop 3 for nothing and settled to −0.20 by loop 10.

**Next, in order:**

1. ~~**Blocked on hardware:** the dark enclosure.~~ **DONE — §1.11** (built, verified dark, curve
   re-derived, `mean_px` gate re-checked). Superseded again by **§1.13**, which lit it.
2. ~~The matched `?cal=0` control, after the enclosure.~~ **NOT DONE — see §1.12.** Arm A ran and
   is a valid sealed-dark result; arm B died with a reboot; and §1.13 then changed the hardware,
   so **both** arms must be re-run. Blocked on: lighting the three slaves, and restoring the
   master to USB power.
3. ~~The node-drop test (CLAUDE.md item 2)~~ **DROPPED by user decision, 2026-07-26.** The
   node-drop, camera-fault/reboot and camera-stall-abort paths will not be exercised — research
   instrument, not a commercial application. Unverified by choice. Do not re-propose.

### 1.11 The enclosure exists — and it overturns §1.9's curve (2026-07-26, later)

A LEGO dark box now covers all four cameras on the bench. Measured on firmware `95ffe4a`; all
four nodes were reflashed first, because the master had been running `af20f90-dirty`, which
pre-dates the two §1.10 fixes — one of which is exactly the sweep's bias reporting.

**Verifying the enclosure — the flatness, not any single reading, is the proof:**

| exposure | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 |
|---|---|---|---|---|---|---|---|---|
| `mean_px` bench (§1.9) | 2.6 | 3.3 | 4.9 | 8.6 | 16.9 | 33.8 | 68.6 | 140.8 |
| `mean_px` enclosed | 3.06 | 3.09 | 3.15 | 3.25 | 3.47 | 3.90 | 3.20 | 3.34 |

A **128× change in integration time moves `mean_px` by 1.27×**, where on the bench it moved 54×.
The residual ≈3.1 is the sensor's exposure-independent black level plus read noise, not light.
**The enclosure is dark.** (A single reading would not have shown this: `mean_px` at exposure 16
read 3.52 idle vs 4.89 in the morning bench sweep — only 1.4× apart, which on its own looks like
a partial enclosure. The exposure sweep is what separates offset from light.)

**1. §1.9's bias-vs-exposure curve does not survive, because it was largely a curve in light.**
Enclosed, bias is **U-shaped with a shallow optimum at 64–128**, degrading at *both* ends:

| exposure | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 |
|---|---|---|---|---|---|---|---|---|
| bias − 0.5 | −1.34e-3 | −1.21e-3 | −1.14e-3 | −1.06e-3 | −5.8e-4 | **−4.6e-4** | −2.06e-3 | −2.32e-3 |
| `zero_diff` | 16.1 % | 16.0 % | 15.7 % | 15.2 % | 14.4 % | 13.1 % | 15.8 % | 15.5 % |
| verdict | BIAS | BIAS | BIAS | BIAS | pass | **pass — chosen** | BIAS | BIAS |

The high-exposure failures can no longer be blamed on a light floor — `mean_px` is 3.2–3.3 there.
Only 64 and 128 pass; every other rung fails BIAS (1e-3). SE(bias) at these window sizes is
≈1.7e-4, so −1.2e-3 is ≈7 σ: the failures are real, not sampling noise. Autocorrelation is a
non-issue throughout, 0.0002–0.0010 against a 0.01 tolerance — passing by 10×.

**2. The `mean_px < 64` leak gate is now entirely non-binding** — 16.4× headroom at the worst
rung, where on the bench it was *the* binding constraint on the master's ladder. **BIAS is now the
sole binding gate.** The threshold also no longer does the job it was chosen for: at 64 it would
not notice the room lights until the box was practically open. Re-derived recommendation: with a
dark floor of 3.1–3.9, **8.0** keeps ≈2× headroom over the floor and would catch a leak an order
of magnitude earlier. **Not applied** — changing a gate constant is a decision, not a measurement.

**3. The enclosure bought stability and cost raw LSB uniformity.** At exposure 128, `zero_diff`
went 5.1 % (bench) → 13.1 % (enclosed) and the best achievable bias went −7.8e-5 → −4.6e-4.
Photon shot noise was doing useful whitening work; without photons the LSB is driven by a smaller
read-noise distribution, more pixels land on the same quantisation level, and the XOR fold carries
more of the load. **The source is now predominantly sensor read/thermal noise rather than photon
shot noise.** That is consistent with the "dark-frame" design as written, but the proportions have
changed, and claims about *photon* entropy should now say so. Both are physical; they are not the
same physics.

Other results: all four nodes certified (`cam_cal=1`), choosing **128 / 32 / 64 / 512** — still
different per node, still by design. Per-node bias 0.499545 / 0.499111 / 0.500000 / 0.499970, all
inside 1e-3. Sweep cost 25.0 s. Step 0 against the ladder's own 16 rung: bias −1.23e-3 vs
−1.14e-3, `mean_px` 3.14 vs 3.15 — the `camera_stats_reset()` proof holds again.

### 1.12 The matched `?cal=0` control (2026-07-26)

⚠ §1.10's calibrated 10-loop session **cannot serve as the control's comparison arm** — it ran in
bench light, which §1.11 shows the enclosure invalidates. The control therefore needs a *fresh
calibrated arm under the enclosure*: two sessions, not one. Both arms are 5 loops × 430 runs
Eurojackpot, 4 nodes, **unattended** (no `?focus=`), run back to back so the only difference
between them is `cal=30000` vs `cal=0`. Unattended on both sides keeps the pair internally valid
per the attended/unattended pooling rule; it does weaken comparability to §1.10, which ran with a
live display, and the 5-loop geometry is deliberately not §1.10's 10-loop one.

**Arm A (calibrated, sealed dark, 5 × 430) — COMPLETE and valid for its conditions:**

| loop | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| σ | 1.1526 | 1.0588 | 1.0584 | 1.0240 | 1.1038 |

**Mean σ = 1.0795 ± 0.0222 — 3.6 SE above unity.** §1.10 (open bench) was 1.015 ± 0.012, consistent
with 1; the difference is 0.065 ± 0.025, i.e. **2.6 SE. Sealing the box made σ significantly worse.**
Worst pair 0-1 (master↔.155) r = +0.0630, |r|√n = **2.92** over 2150 runs — under the threshold of
3, so it never flagged, while σ went to 1.15. §1.10's worst was 0.95 over twice the runs.

Per-loop drift of the raw offset, regressed on loop index (computed by hand — `DRIFT_MIN_LOOPS` is
6, so a 5-loop session publishes nothing):

| node | slope (z/loop) | t (3 df) |
|---|---|---|
| master | **−0.334** | **−4.75** |
| .155 | −0.502 | −2.19 |
| .103 | −0.024 | −1.37 |
| .145 | +0.036 | +0.67 |

Only the master is significant (crit. 3.18 at 5 %); it walks +0.97 → −0.41 over 70 min, against
§1.10's −0.0054 z/loop at t = −0.20. Dropping loop 3 leaves the slope identical and raises |t| to
12, so it is not a single-point artifact. Loop 1 is the worst loop (σ 1.1526) and decomposes
exactly: 49 % of the excess variance is .155's own over-dispersion (σ 1.2798 at exposure 128,
which per-loop calibration then moved to 64, dropping it to 1.0253), 49 % is inter-node
correlation, 2 % the other nodes.

**The correlation is NOT rail-borne**, and arm A is the only session that can say so, because it
ran with the master still on USB and the three slaves on PoE:

| pairs | r | mean |
|---|---|---|
| master ↔ slaves (0-1, 0-2, 0-3) | +0.0630, +0.0142, −0.0095 | **+0.023** |
| slave ↔ slave (1-2, 1-3, 2-3) | +0.0018, +0.0287, +0.0418 | **+0.024** |

Identical means, and the largest single pair involves the node on the *isolated* rail. Combined
with the master's drift, the best reading is **thermal**: four self-heating boards sealed in a box
with no airflow. The enclosure traded a light confound for a thermal one.

**Arm B (`cal=0` control) — VOID.** The master rebooted mid-session while the LED was being
fitted (USB unplugged), taking the session with it. Nothing to salvage.

⚠ **Arm A is now also void as half of a matched pair** — not because it is wrong, but because it
measured a sealed-dark enclosure, and §1.13 changed the hardware. Both arms must be re-run under
the final lighting and power topology.

### 1.13 Lighting the enclosure (2026-07-26, evening)

§1.11 concluded the box was dark, which cost raw LSB uniformity, and §1.12 found sealing it cost σ
as well. The fix is **controlled light, which is what §1.9's "under controlled light" always meant
and is not the same as darkness.** An LED was added to the master. Three failure modes were found
before it worked, and the order matters:

**1. LED on the ESP's VSYS pin, PWM-dimmed — catastrophic, and the mechanism is *conducted*, not
optical.** Bias went to −4.33e-3 (4× worse than darkness, 11× worse than open bench), σ 1.05–1.07,
and **0 of 9 rungs certified**. The initial diagnosis — PWM chopping the light so consecutive
frames see different illumination, breaking `f[2k+1] − f[2k]` — was **wrong**: `mean_px` was
2.4–3.5 throughout, i.e. there was never enough light to chop. Moving the LED to its own supply
restored 8 of 9 rungs *while the light level barely changed*. **The damage was PWM current on the
rail feeding the sensor's analog supply.** Never power illumination from a node's VSYS.

**2. Too dim to measure.** With a clean supply the LED's contribution was still not distinguishable
from zero: `mean_px` stayed flat at 3.3–4.1 across the ladder (1.25×, the dark signature), and at
exposure 128 read *lower* than sealed dark. Sweep-to-sweep variation is the same size as the
signal at that level. Needed ~35–95× more light, not the 5–8× first estimated from a `/diag`
reading that was contaminated by the VSYS window.

**3. Too bright.** `mean_px` 159 at exposure 256, `zero_diff` **up** to 20.4 % and σ to 6.6 —
saturation, not noise: clipped pixels do not change between frames, so they diff to zero.

**Final master setting (verified):** light stable to **1.0 % over 56 s** (15.48–15.64), no flicker.
LED output falls ~12 % over the first minutes as the junction warms — let it settle before
measuring.

| exposure | `mean_px` | bias − 0.5 | `zero_diff` | σ | autocorr | verdict |
|---|---|---|---|---|---|---|
| 4 | 5.6 | −1.59e-3 | 14.6 % | 1.005 | 0.0004 | BIAS |
| 8 | 8.7 | −1.06e-3 | 11.8 % | 1.002 | 0.0007 | BIAS |
| **16** | **15.6** | **−3.7e-4** | **9.2 %** | 0.996 | 0.0004 | **chosen** |
| 32 | 32.9 | −2.1e-4 | 6.6 % | 0.994 | 0.0004 | passes |
| 64 | 68.7 | **−4.8e-5** | 5.9 % | 0.998 | 0.0009 | **LIGHT — by 7 %** |
| 128 | 118.6 | −8.09e-3 | 11.4 % | 1.906 | 0.0097 | saturating |
| 256 | 165.3 | −4.44e-2 | 23.6 % | 6.314 | 0.0887 | saturating |
| 512 | 207.9 | −8.37e-2 | 37.7 % | 8.937 | 0.1661 | saturating |

`mean_px` now spans 25.7× across the ladder where sealed-dark gave 1.27× — photons are reaching
the sensor and shot-noise whitening is back (`zero_diff` 9.2 % at exposure 16 beats the open
bench's 10.8 %). **This is the best sustained state the master has been in.**

**Two software policies now cost a factor of 7 in bias, and neither is physics:**

- **The `mean_px < 64` gate excludes the best rung.** Exposure 64 measures bias −4.8e-5 — matching
  §1.9's open-bench best — with σ 0.998 and autocorr 0.0009, no quality failure at all. It is
  rejected on light level alone, by 7 %. The gate was written when *all* light was an accidental
  leak; with deliberate illumination its semantics have changed. True saturation onset lies
  between 68.7 (clean) and 118.6 (σ 1.91). ⚠ Still **not changed** — a decision, not a measurement.
- **Selection takes the *fastest* passing rung, which is degenerate.** §1.10 established the bit
  rate is CPU-bound; the sweep measures 3.217–3.293 Mbit/s across exposure 4→512, a 2.4 % spread,
  so the tie-break among passers is noise. It picked exposure 16 (bias −3.7e-4) over 32
  (−2.1e-4). Across arm A the master wandered 128 → 256 → **8** → 128 → 128 for the same reason,
  once choosing a rung a standalone sweep had *failed*. **Recommendation: select the lowest
  |bias − 0.5| among passing candidates** ([camera.c:836](../components/elotto_camera/camera.c#L836)).
  Note that dimming the light cannot recover this on its own — selection always sits at the dim
  end of the passing range, wherever that range is put.

⚠ **Hardware state at the end of this session:**
1. ~~Master and slave1 lit; slave0 and slave2 still dark.~~ **RESOLVED — all four lit**, exposures
   16 / 16 / 32 / 16 (master / .103 / .145 / .155), `mean_px` 15.5 / 36.8 / 22.0 / 33.0, every node
   certified with σ within 0.003 of 1.000. See §1.14.
2. **All four nodes are on PoE, and this is now permanent (user decision, 2026-07-26).** The
   master's separate USB supply — the PLAN_NETWORK Risk 1 control — came off when its LED was
   fitted and **will not be restored; do not re-propose it.** Consequence: inter-node correlation
   can no longer be attributed to the shared rail versus anything else, because there is no node
   on an independent rail to contrast against. **Arm A above is the only measurement that can ever
   separate them on this rig**, and it says the correlation is *not* rail-borne. It cannot be
   repeated.

### 1.14 The control pair, re-run under light — TASK 1 CONTROL PASSED (2026-07-26, night)

Both arms re-run after §1.13 lit all four cameras. 5 loops × 430 runs each, unattended, back to
back so both sit in the same thermal state. Entry exposures 16 / 16 / 32 / 16
(master / .103 / .145 / .155) — `cal=0` does not log them, so they are recorded here.

| loop | 1 | 2 | 3 | 4 | 5 | **mean σ** |
|---|---|---|---|---|---|---|
| arm A `cal=30000` | 0.9863 | 1.0102 | 0.9635 | 1.0101 | 1.0500 | **1.0040 ± 0.0144** |
| arm B `cal=0` | 0.9847 | 0.9706 | 0.9743 | 1.0226 | 1.0451 | **0.9995 ± 0.0147** |

**A − B = +0.0046 ± 0.0206 = 0.22 SE. No detectable difference.** Per-loop camera calibration is
**statistically neutral** — it adds no variance of its own and removes none. That was the one way
Task 1 could have done real damage, and it does not. Both arms sit within 0.3 SE of unity, and both
SDs across loops (0.0322, 0.0328) fall *below* the 0.0341 expected from sampling alone, so neither
carries a hidden variance component.

| gate | result |
|---|---|
| `loop_sigma` ≈ 1 over a calibrated multi-loop session | ✅ 1.0040 ± 0.0144 |
| matched `?cal=0` control run and compared | ✅ 0.9995 ± 0.0147, difference 0.22 SE |
| pairwise matrix stays clean | ✅ arm B worst \|r\|√n = **1.27** over 2150 runs (threshold 3) |
| per-loop cost inside budget | ✅ ~24.2 s/loop |
| chosen settings in `/loops` | ✅ arm A, all 5 loops |
| a node that does not answer is dropped | ⬜ never exercised — **dropped by decision**, CLAUDE.md item 2 |

**Lighting the enclosure fixed everything sealing it broke.** Three independent signatures moved
together, in the same direction:

| | sealed dark (§1.12) | lit (§1.14) |
|---|---|---|
| mean per-run σ | 1.0795 ± 0.0222 | **1.0040 ± 0.0144** |
| worst \|r\|√n | 2.92 | **1.27** |
| master drift | −0.334 z/loop, t = **−4.75** | **−0.075 z/loop, t = −2.62** |

Arm B's matrix is the cleanest measured on this rig — three of six pairs are *negative*. The
master's drift is 4.5× smaller and below the |t| > 3 flag; no other node is significant. The
thermal explanation remains **unproven** — enclosure temperature was never measured — but it now
has three signatures agreeing across two conditions.

Other results: `net_retries`/`net_lost`/`net_stale` all **0** across both arms, 4/4 nodes
throughout, no faults, stalls or reboots. Window 1028–1034 ms, gap 354–359 ms against the 350 ms
constant. `best_z` = 3.184 against an expected max |z| ≈ 3.0 for 430 draws — a null instrument.

⚠ **Arm A's pairwise matrix was lost.** The orchestration started arm B before `/loops` and
`/status` were captured, and a new session resets `PairAcc` and `loop_hist`. Arm A's five σ values
and its loops 1–2 per-node detail survive; loops 3–5 per-node data and the whole arm-A matrix do
not. The σ comparison above is complete, but the *correlation* comparison rests on arm B alone. σ is
the more sensitive of the two and agrees, so this is inference rather than measurement.
**Lesson: capture `/loops` and `/status` at arm completion, before starting anything else.**

**Next, in order:**
1. ~~**Selection criterion**~~ **DONE (§1.15)** — now lowest |bias − 0.5| among passing candidates.
2. ~~**The `mean_px < 64` gate**~~ **DONE (§1.15)** — raised to 100.0.
3. **The attended-vs-unattended comparison** — `focus=1` against a matched `focus=0` control, on a
   statistic chosen *before* looking. This is the actual experiment, and it has never been analysed.
   ⚠ Deferred by the user, 2026-07-27. Do not start it unasked.

### 1.15 The control pair re-run under the final optics (2026-07-27)

Everything §1.14 measured was superseded at once: the calibration **selection rule** changed
(lowest |bias − 0.5|, not fastest), the **light gate** rose (`CAL_MAX_MEAN_PX` 64 → 100), the
**optics** changed (LEDs now fixed to each camera and shielded — master `mean_px` 45.7 at exposure
16, against §1.13's 15.6), and `main/` was **refactored into four files** with the z-score
primitive moved into a shared component. So the pair was re-run: 6 loops × 430 runs, Eurojackpot,
4 nodes, unattended, back to back. 6 loops rather than 5 because `DRIFT_MIN_LOOPS` is 6 — a 5-loop
session publishes no drift figures at all, which is why §1.12 and §1.14 both had to compute the
slope by hand.

| loop | 1 | 2 | 3 | 4 | 5 | 6 | **mean σ** |
|---|---|---|---|---|---|---|---|
| arm A `cal=30000` | 1.0313 | 0.9903 | 1.0452 | 1.0338 | 0.9608 | 0.9839 | **1.0075 ± 0.0138** |
| arm B `cal=0` | 1.0055 | 0.9927 | 1.0134 | 0.9745 | **1.4179** | 1.0109 | **1.0691 ± 0.0700** |

**A − B = −0.0616 ± 0.0713 = 0.86 SE.** Excluding arm B's loop 5 (below): B = 0.9994 ± 0.0072 and
**A − B = +0.0081 ± 0.0156 = 0.52 SE**. Neutral either way, so **the Task 1 conclusion survives the
new selection rule, the new gate, the new optics and the refactor.**

| gate | arm A | arm B |
|---|---|---|
| mean σ within 2 SE of 1 | ✅ 0.55 SE | ✅ 0.99 SE (see caveat) |
| worst \|r\|·√n < 3 (n = 2580) | ✅ **1.04** | ✅ **0.95** |
| `net_retries`/`lost`/`stale` | ✅ 0 / 0 / 0 | ✅ 0 / 0 / 0 |
| faults, stalls, reboots | ✅ none, 4/4 throughout | ✅ none, 4/4 throughout |
| per-node bias within 1e-3 | ✅ worst .103 at 0.499188 | ⬜ not measurable — `cal=0` never populates `cam_bias` |
| window / gap | 1027.1 / 348.0 ms | 1027.2 / 348.0 ms |

⚠ **Arm B's σ gate is a weak pass.** It clears 2 SE only because loop 5 inflates its own SE
five-fold (0.0138 → 0.0700). A single bad loop widens the error bar that judges it, so this gate
cannot detect a single bad loop *by construction*. Read the per-loop column, not the mean.

**The refactors are clean.** Arm A ran 2580 runs on the split codebase and is statistically
indistinguishable from §1.14's arm A on the old one:

| | §1.14 arm A (old code) | §1.15 arm A (nodes.c/focus.c/elotto_gcp) |
|---|---|---|
| mean σ | 1.0040 ± 0.0144 | 1.0075 ± 0.0138 |
| worst \|r\|·√n | 1.27 | 1.04 |

**Node .145 lost a loop, and it was NOT correlation.** Arm B loop 5:

| node | master | **.145** | .103 | .155 |
|---|---|---|---|---|
| σ | 0.9817 | **2.2673** | 0.9863 | 1.0377 |
| mean offset | +0.0044 | **+2.0973** | −0.2099 | +0.1084 |

`√(Σσᵢ²/4)` = **1.428** against the observed **1.4179** — the combined excursion is fully accounted
for by .145's own variance with **no inter-node term**, and the pairwise matrix stayed clean
(worst 0.95). That is structurally unlike §1.12's sealed-dark result, where correlation supplied
about half the excess. `.145` held `cam_mbit` 3.417 with zero stalls throughout: the camera kept
delivering bits at full rate, and it was their *quality* that moved. It partly recovered in loop 6
(σ 1.0433, offset still +1.1168).

⚠ **Do not conclude that calibration prevented this.** Three reasons: n = 1; the arms are
**ordered, not randomised**, so B is always the later and warmer one; and arm A had its own
single-loop anomaly — `.103` at mean offset **+2.3720** in loop 1 — which merely happened to be an
offset rather than a variance excursion, and studentization removes offsets exactly.

**But the design point is real, and it is a criticism of this experiment, not of the firmware:**
if per-loop calibration protects anything, it protects the **tail** — rare bad loops — and a
6-loop comparison of *mean* σ cannot see that. §1.14 and §1.15 both answered "does calibration add
variance?" (no). Neither has ever asked "does calibration reduce the rate of bad loops?", which
needs many more loops and a count of excursions, not a mean.

**Drift, published for the first time** (6 loops clears `DRIFT_MIN_LOOPS`):

| arm | slope (z/loop) | t | flagged? |
|---|---|---|---|
| A | +0.0208 | 0.59 | no |
| B | +0.0248 | **3.79** | **yes, \|t\| > 3** |

Near-identical slopes, opposite verdicts — arm A's baseline offsets scatter far more
(−0.62 … +0.04) than arm B's (−0.19 … +0.08), so the same trend clears significance only in B.
Both are **positive**, where §1.12's sealed-dark master drifted **−0.334** z/loop. Worth watching,
not yet worth explaining.

⚠ **Node discovery order is not stable between sessions.** Arm A enumerated
`self/.103/.145/.155`, arm B `self/.145/.103/.155`. The `nodes[]` index is therefore **not** a node
identity across sessions, and reading `/loops` with the wrong arm's ordering misattributes
per-node results to the wrong hardware. Always map through `nodes[].ip`.

---

## Workflow

Planning/architecture: Fable/Opus — this document is the contract. Implementation: Sonnet, one
task per session. Escalate back if a gate fails twice or a decision above is missing. Commit at
every green gate; the master and slave repos must be committed and flashed together whenever the
shared `components/` or the wire protocol changes.

**Session prompt** (Task 1 is complete; this is the prompt for what follows it):

> Task 1 of docs/PLAN.md — per-loop camera calibration — is built and its gate passed. Start by
> reading §1.10: it records the 10-loop result and, importantly, what is still **provisional**
> because the cameras had no dark enclosure when it was measured.
>
> First check whether the enclosure now exists (ask me, or look at the master's `mean_px` in
> `/diag` — bench light ran 5–10 at exposure 16). If it does: re-derive the bias-vs-exposure
> curve under controlled light, re-check the `mean_px < 64` leak gate against it, then run the
> deferred matched `?cal=0` control. If it does not: leave all light-dependent work alone and do
> the two untested safety paths instead — node-drop and camera-fault/reboot (CLAUDE.md open
> item 2).
>
> Everything you need is in docs/PLAN.md and CLAUDE.md.

Start every session by re-reading **§1.10** and updating it at the end, so the next one never has
to re-derive the state.

§1.7's decisions are settled — do not re-litigate them, **except decision 3 (the XOR fold), which
was WITHDRAWN on measured evidence**; §1.10 and the comment in `camera_calibrate()` say why, and
that withdrawal is itself now settled. Entropy is photons only: the on-chip TRNG was removed from
both firmwares (see the CLAUDE.md noise-source section) and must not be reintroduced in any form.
