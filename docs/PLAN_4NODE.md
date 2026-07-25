# PLAN: 4-Node GCP Array with OV5647 Camera Entropy

Status: **Phases 0–3 DONE** (2-node system, camera entropy on both nodes, v2.1). This document
is the contract for the **noise source and the statistics** and stays authoritative for those.

**Scale-out moved out of this document.** It was written when only 2 boards existed and the
4-node array was blocked on hardware; that hardware has since arrived (4× ESP32-P4-ETH, one
OV5647 each, 4-port PoE switch), which reopened the transport decision. Scaling, transport,
provisioning and firmware delivery now live in [`PLAN_NETWORK.md`](PLAN_NETWORK.md) — Phase 4
below is superseded and kept only as a record.

Implementation is phased; each phase is one focused coding session with clear acceptance
criteria. Do not start a phase before the previous one's gate passes.

**Phase 5 (Focus display)** is new and not yet started — it changes the measurement protocol
and the UI, so it lives here rather than in `PLAN_NETWORK.md` (whose "Phase D" is the 4-node
scale-out). It touches the master/slave run-length exchange, so read its "coordinate with
Phase C" note before implementing either.

## Goal

Replace the on-chip TRNG (~0.5 Mbit/s effective entropy, whitened, opaque) with **OV5647
dark-frame noise** (photon shot + read noise ≈ quantum-origin, raw, ~2–5 Mbit/s clean per
node) on both existing nodes, keeping the proven master/slave architecture at 2 nodes
(SNR ×√2, unchanged from today). All existing statistics machinery (studentization,
permuted order, Stouffer accumulation, coverage, independence checks) stays unchanged —
only the bit source changes. The combine/PairStats math is written generally (√n over n
healthy nodes) so it extends to 4 nodes later without rework, but n=2 is the only
configuration built and tested now.

## Architecture decisions (made, do not re-litigate)

- **Topology: UART point-to-point, unchanged.** Master (COM4) + slave (COM6) on UART1
  (GPIO14/15, 460800 baud) — the wiring already in place. The star topology for a 3rd/4th
  slave (UART2/3) is deferred to Phase 4; no GPIO-matrix routing work happens now.
- **One entropy source per node** (its own camera). Never share a noise source between
  nodes — it would break independence by construction.
- **Camera replaces TRNG behind the same interface.** `gcp_zscore_raw()` keeps its
  segment math; only the word source changes. TRNG register remains available as fallback
  and for A/B comparison in /diag.
- **Combine:** `z = Σ z_i / √n` over master + all healthy slaves (generalizes the current
  ÷√2; with 2 nodes today this is exactly ÷√2). Per-node health degradation exactly as
  today (drop node, adjust √n).
- **Run length becomes a config knob.** With honest (non-oversampled) bits, a run no longer
  needs 6.4 Mbit. Target ~0.5 s/run initially (≈1–2 Mbit/run, i.e. NUM_SEGMENTS becomes
  source-dependent) so Eurojackpot loops stay ~1.5 h. Statistical power per second is
  rate-limited either way; run length only sets granularity.

## Phase 0 — Camera bring-up + validation (master, COM4)

- Component: `espressif/esp_video` (ESP-IDF v6, P4 MIPI-CSI) + OV5647 sensor driver
  (`esp_cam_sensor`). Camera is already wired to the master's CSI connector.
- Sensor config: RAW8 (or RAW10→8), 640×480 or 800×640 @ max stable fps, **AE/AGC/AWB
  off**, fixed max analog gain, short fixed exposure. Lens capped + taped, opaque housing.
- Extraction pipeline (camera task → ring buffer):
  - Non-overlapping frame pairs: diff = f[2k+1] − f[2k] per pixel (cancels FPN exactly).
  - Take LSB of each diff, pack 32 bits → uint32 words into a ring buffer (≥ 64 KB).
  - Optional XOR-fold (bit ⊕ next-bit, halves rate) — only if autocorrelation gate fails.
- Extend `/diag` with per-source stats (TRNG vs camera): bit bias, per-run σ over ≥200
  mini-runs, lag-1..4 word autocorrelation, sustained Mbit/s, stuck-frame counter, and a
  **light-leak check** (mean raw pixel level must sit at the black floor; warn above
  threshold).
- **Gate:** |bias−0.5| < 1e−3, |lag-1 autocorr| < 0.01, σ within 1±0.05, no stuck frames,
  light-leak pass, sustained ≥ 2 Mbit/s.

**Status: PASSED** (2026-07-25, master/COM4, RAW8 800x800 @ 50fps, exposure 16, gain 1023,
XCLK unwired — the RPi-style OV5647 module clocks itself; SCCB on GPIO8/7):

| bias | σ | Mbit/s | lag-1..4 r | stuck | mean px |
|------|---|--------|------------|-------|---------|
| 0.499844 | 0.9975 | 3.419 | ≤ 0.0002 | 0 | 3.56/255 |

Three findings worth carrying forward:
- The raw (unfolded) LSB stream is biased 0.4849 — a ~3% excess of even diffs that a
  symmetric noise distribution cannot produce (only 12.6% of diffs are 0, so the noise
  spans ~2.7 ADU and quantization starvation is ruled out). Most likely ISP digital
  processing making the raw LSB non-uniform. XOR-folding masks it
  (`CONFIG_ELOTTO_CAM_XOR_FOLD`, default y); it does not remove the cause. Attacking the
  source (RAW10, or bypassing ISP digital gain) is open work — turn the fold off to A/B it.
- Even folded, the residual bias is ~3σ from 0.5 given 105.9 Mbit (SE ≈ 4.9e-5). Inside the
  gate by 6×, but not a clean 0.5.
- Throughput is dominated by PSRAM bandwidth, not the sensor: holding both frames of a pair
  dequeued instead of memcpy'ing one aside took the rate from 1.8 to 5.7 Mbit/s (3.2×).

## Phase 1 — Entropy abstraction (master)

- `noise_word()` interface behind `fast_rng()`'s call sites; runtime source select
  (camera / TRNG) + automatic fallback to TRNG with a status flag if the camera stalls.
- Blocking semantics: if the ring buffer underruns, the GCP task waits (vTaskDelay) — never
  reuse or fabricate bits.
- Make segments-per-run a per-source constant (TRNG: 32000 as today; camera: sized for
  ~0.5 s/run at measured rate).
- **Gate:** full 6/49 quick session (Loops=3, Runs=200, camera source) with per-run
  σ ≈ 1, clean pair_r vs the TRNG-based slave, no underruns at sustained rate.

**Status: PASSED** (2026-07-25, 6/49, Loops=3, Runs=200, `?src=1`, ~22 min, 5.34 Gbit):

| loop_sigma (3 loops) | pair_r (n=600) | sigma_m / sigma_s | stalls | fallback |
|----------------------|----------------|-------------------|--------|----------|
| 0.9982 / 0.9983 / 0.9917 | −0.0191 (\|r\|·√n = 0.47) | 1.0178 / 0.9935 | 0 | no |

- `?src=1` on /start selects camera, `?src=0` TRNG; /status reports `src` and
  `src_fallback`, /diag adds ring `drops`/`waits`/`stalls`.
- High `drops` (~6.6M words) are normal and not a defect: production (3.43 Mbit/s) and
  consumption (~3.2 Mbit/s in-run) are near-balanced, so the ring fills during inter-run
  gaps and surplus bits are discarded. Unread words are never clobbered, never reused.
  `waits` is likewise normal backpressure. Only `stalls` indicates a real underrun.
- The Fisher–Yates shuffle deliberately stays on the TRNG: measurement *order* is
  administrative randomness, not measured data, and must not consume rate-limited
  camera entropy.
- **Residual bias propagates to a per-run z offset of ≈ −0.33** (1.29e-4 bias × 200
  bits/segment × √8000 segments). It is a 19σ deviation at this sample size, not noise.
  Harmless here only because `studentize()` removes a constant per-loop offset exactly
  and the per-loop permutation prevents coherent accumulation. Drift is the one form this
  correction does not fully absorb — Phase 3 therefore made the offset *measurable* per loop
  (`/loops`, `drift_slope`/`drift_t`). The long session that would have exercised it was
  cancelled, so this remains measured-but-not-yet-observed; any future multi-loop run settles
  it automatically.

## Phase 2 — Camera on slave (2-node parity)

- Slave firmware (repo `elotto_slave`): integrate the same camera pipeline (component is
  shared from the master repo via `EXTRA_COMPONENT_DIRS=../elotto/components` — both repos
  are siblings on disk; note this in both READMEs). Requires a 2nd OV5647 wired to the
  slave's (COM6) CSI connector.
- Both nodes now run camera-sourced entropy in parallel over the existing UART1 link — no
  new UART wiring, no `slaves[]` array yet (still exactly one slave).
- PairStats stays the single (master, slave) tracker it already is; verify pair_r ≈ 0 and
  sigma_m/sigma_s ≈ 1 with both sides on camera source.
- UI: stats line reflects camera source per node (reuse the existing master/slave σ row;
  no "N-node" badge needed until Phase 4).
- **Gate:** 2-node quick session, both nodes on camera; pair_r ≈ 0; combined σ ≈ 1; either
  side falling back to TRNG (camera stall) degrades gracefully with a UI flag, not a crash.

**Status: PASSED with two caveats** (2026-07-25, 6/49, Loops=3, Runs=200, both `src=cam`,
35.4 min):

| pair_r (n=600) | σm / σs | loop_sigma (3 loops) | stalls | fallback |
|----------------|---------|----------------------|--------|----------|
| −0.0201 (\|r\|·√n = 0.49) | 1.0141 / 1.0356 | 0.9721 / 0.9668 / 1.0992 | 0 | none |

1. **Combined σ is looser than Phase 1** (which gave 0.998/0.998/0.992). Loop 3 at 1.0992 is
   ~2σ high for n=200 (SE ≈ 0.05) — consistent with sampling noise, but the drift direction
   matches the slave's dirtier camera (σs > σm). Re-check in Phase 3.
2. **The graceful-fallback criterion is UNTESTED.** No camera ever stalled, so the
   degradation path has never been observed under a real stall. See "Fallback policy" below.

Implementation notes:
- Camera extraction now lives in `components/elotto_camera/`, shared with the slave via
  `EXTRA_COMPONENT_DIRS=../elotto/components` — one source of truth, byte-identical
  extraction on both nodes.
- **Producer/consumer priority is load-bearing.** The extraction task (priority
  `ELOTTO_CAM_TASK_PRIO` = 4) is CPU-hungry; a consumer running *below* it gets starved and
  a run takes 5.1 s instead of 0.47 s. The slave's command loop is `app_main`, whose
  priority IDF hardcodes to 1, so it calls `vTaskPrioritySet(NULL, ELOTTO_CAM_TASK_PRIO+1)`.
  Diagnostic signature of the bug: `drops` huge with `waits == 0` (consumer never waited,
  so it is the bottleneck).
- Protocol extended: `M` replies `Z:<float>,<C|T>`; the tag is after the float so `atof()`
  still parses and a pre-camera slave stays compatible. New `D` command returns slave camera
  diagnostics.
- Sensor register writes are verified by read-back (`cam_verify_regs`): exposure, gain and
  AEC/AGC-manual all confirmed applied and surviving STREAMON on both nodes.

**The two cameras are not equally clean, and it is not configuration.** Identical settings
(exposure 16, gain 1023, verified by read-back) yet:

| | master | slave (14.6 Gbit) |
|---|--------|-------------------|
| mean_px | 6.80 | 2.84 |
| zero_diff | 9.4% | 16.2% |
| bias deviation | 3.7e-5 | 8.6e-4 (208σ, stable) |

Bias tracks `mean_px` → `zero_diff`: more photons → more shot noise → wider noise
distribution → more uniform LSB. Two hypotheses were tested and **refuted**: sub-ADU
quantization starvation (zero_diff is only ~10-16%, not the 40-70% required) and sensor
warming (82 min under load moved slave mean_px 2.67 → 2.84, i.e. not at all). Remaining
explanation is a per-unit difference in light reaching the sensor — meaning the *master* is
likely the less light-tight of the two, and that is why its bits are better. Flicker is
ruled out (autocorrelation 0.0000 on both). If tuning this: give the slave *more* light, not
less. Photon shot noise is Poisson arrival statistics — quantum-origin, and arguably a
better source than dark current.

### Fallback policy — DECIDED: (a) abort on stall

**A camera stall aborts the session. The TRNG is never silently substituted.**

Rationale: the premise of this plan is *replacing* the opaque whitened TRNG with raw quantum
noise. Substituting it back mid-session changes the physics being measured, and a
session-level flag cannot say *which* runs were affected — a stall at run 3 of 2560 would
leave 2557 TRNG-sourced runs inside a session still labelled "camera". Losing the run is
cheaper than silently contaminating it.

Implemented (`noise_camera_stalled()` in sensor.c):
- local stall → `g_status.noise_stalled = true`, `abort_requested = true`;
- camera requested but not streaming at session start → same, so a "camera" session never
  starts on TRNG bits (`noise_source_begin()` runs *after* `abort_requested` is cleared,
  or the reset would wipe the flag);
- **slave stall** → the slave reports `T` in its `Z:` reply and the master aborts too,
  since the combined z would otherwise mix sources;
- the slave re-arms its camera on each `B` (session start), so one transient stall does not
  latch it to TRNG until power-cycle and doom every later session to an instant abort;
- UI shows "⚠ camera stalled – aborted"; `/status` exposes `src_stalled`.

Note this differs from what a multi-node array will want. With n ≥ 3 nodes, option (b) —
drop the stalled node and combine over n−1 (no ÷√n on the dead node) — keeps the session
alive without mixing sources, and should be revisited in Phase 4. With 2 nodes there is no
meaningful "degrade": losing one halves the array and changes the instrument, so aborting is
the honest response.

**Still untested against a real stall.** No camera has ever stalled in testing; the healthy
path was verified not to false-positive (a camera session starts and runs normally), but the
abort path itself has only been reasoned about, not observed. Unplugging a CSI ribbon
mid-session would test it.

## Phase 3 — Drift instrumentation + docs

- ~~20 h Eurojackpot cumulative session~~ — **cancelled** (2026-07-25, user decision). Occupying
  both nodes for a day is not worth it at this stage. What the long session was *for* — being
  able to see drift at all — is delivered by the instrumentation below; running it is now an
  option, not a prerequisite. See "What the cancellation costs" below for what stays unproven.
- README: new "Camera entropy" section (physics, extraction, gates), updated wiring
  diagram/screenshots; CLAUDE.md concept sync; version bump.

**Status: DONE (2026-07-25, v2.1)** — instrumentation, docs, and a functional verification of
both on hardware.

Drift could not be *judged* with what Phase 1/2 published — `loop_sigma` held only the last
loop, and nothing recorded the offset studentization removed. Phase 1 explicitly deferred the
question here ("drift is the one form this correction does not fully absorb"), so Phase 3 built
the instrument to answer it:

- **`record_loop()`** stores one `LoopStat` per completed loop: raw (pre-studentize) per-run
  offset `base` + `raw_m`, combined and per-node means and σ, and camera health at that moment
  for *both* nodes. Lives in **PSRAM** — internal RAM is full with `results[]`, and the few KB
  of extra `.bss` failed the *link*, not the run.
- **`drift_add()`** regresses the master's raw per-run offset on the loop index and publishes
  `drift_slope` (z per loop) and `drift_t = slope / SE(slope)`; |t| > 3 means the trend is real
  rather than noise. Running sums, so the test stays exact past the 128 stored loops.
- **`GET /loops`** serves the whole table (chunked); `/status` adds `loops_done`,
  `drift_slope`, `drift_t`, `off_first`/`off_last`, `sigma_lo`/`sigma_hi`; the results line
  shows `offset a → b · drift ±s z/loop (t = …) · σ lo–hi over N loops` with a ⚠ at |t| > 3.
- Slave per-loop camera numbers come from the existing `D` command, queried once per loop
  while the slave is idle. A missing reply is diagnostics only and never drops the slave —
  losing it would cost the session half its SNR over a failed status query.
- **Entropy selector in the UI**, and `/start` now sets the source *explicitly* (camera by
  default) instead of inheriting whatever the previous session used. The source decides what
  physics is being measured; it must never carry over silently.

**`SCORE_REPS` stays at 10** (lowered from 40; user decision 2026-07-25). Scoring is a
one-time phase costing ~27 min at 40 reps for Eurojackpot, which blocked every test run from
reaching the loops it was supposed to exercise. Consequence, stated plainly: the pool is
selected with per-number SE = 1/√(2·10) ≈ 0.22 instead of 0.11, and in cumulative mode that
pool is locked for the whole session. This changes *which* numbers enter the pool (more
selection noise), **not** the Phase-2 z statistics — studentization, the permuted order and the
Stouffer accumulation are untouched, and the significance line stays honest either way. Raise
it to 40 for any session whose pool choice is meant to be trusted.

Whenever a multi-loop session is run, these are the numbers to read (they are published
continuously, so any session serves as evidence — no dedicated gate run needed):
`loop_sigma` near 1 in **every** loop of `/loops`, not just the last; |drift_t| < 3 on the raw
offset; `cam_stalls` = 0 on both nodes; `pair_r` ≈ 0; and a corrected p consistent with chance
under the null.

**Instrument verified** (2026-07-25, 6/49, Loops=3, Runs=20, both `src=cam`, 7 min — a
functional check of the new plumbing; n is far too small to conclude anything about drift
itself):

| loop | base | raw_m | σ | σm / σs | cam Mbit/s M/S | stalls M/S |
|------|------|-------|---|---------|----------------|------------|
| 1 | −0.8903 | −0.2225 | 0.9785 | 1.017 / 0.851 | 3.487 / 3.221 | 0 / 0 |
| 2 | +0.0658 | +0.2049 | 1.0323 | — | 3.472 / 3.211 | 0 / 0 |
| 3 | −0.2308 | −0.1185 | 1.1623 | — | 3.458 / 3.200 | 0 / 0 |

- `raw_m = base + mean_m` holds on every row; `mean = (mean_m + mean_s)/√2` reproduces the
  published combined mean. The device's `drift_slope`/`drift_t` (0.05202 / 0.24) recompute
  exactly from the served table, and the running-sums regression was separately checked
  against closed-form OLS (agrees to 1e−13; flags a synthetic 0.004 z/loop trend at t = 5.1
  while passing flat noise at t = 1.4).
- `slave_diag()` round-trips: the slave's own camera rate and stall count appear per loop.
- The results line renders `offset −0.223 → −0.118 z/run · drift +0.0520 z/loop (t = 0.2 ok)
  · σ 0.979–1.162 over 3 loops · table`.

### What the cancellation costs

Being explicit, so this is not mistaken for a passed gate:

- **Drift over many hours is measured but not yet observed.** Three loops over 7 minutes say
  nothing about a thermal ramp or a slow sensor trend; `drift_t = 0.24` there is a plumbing
  check, not evidence of stability. The claim "studentization handles the camera's residual
  bias" therefore remains *argued* (it removes a constant offset exactly) rather than
  *demonstrated* over a long run. Any future multi-loop session closes this for free — the
  numbers accumulate automatically and `/loops` keeps the record.
- **σ stability across many loops is likewise unproven.** Phase 2 already flagged loop 3 at
  1.0992 as worth re-checking; `sigma_lo`/`sigma_hi` now make that visible in any session.
- Everything else the long session would have exercised — stall-free operation, `pair_r ≈ 0`,
  honest corrected p — is already covered by Phase 1/2 at n = 600 pairs each.

### Remaining work in this plan

1. **The abort-on-stall path is still unverified** (carried over from Phase 2, and the only
   *safety* claim in this document that has never been observed). Unplug a CSI ribbon mid-run:
   expect `src_stalled`, session ABORTED, UI "⚠ camera stalled – aborted", and — with the slave
   unplugged instead — the same via its `T` tag. Minutes of work, and it is the difference
   between a policy that is implemented and one that is known to fire.
2. **Refresh `docs/ui_done.png` and `docs/coverage_*.png`** from any real session — the current
   images predate the entropy and drift rows in the stats line. `docs/ui_start.png` is already
   updated (Entropy selector).
3. **Optional, open since Phase 0: attack the residual LSB bias at the source** rather than
   masking it with the XOR-fold — RAW10 instead of RAW8, or bypassing ISP digital gain. A/B it
   by turning `CONFIG_ELOTTO_CAM_XOR_FOLD` off; success would double the honest bit rate as a
   side effect.
4. **Optional: even out the two cameras.** The slave is the dimmer, dirtier one (Phase 2:
   `mean_px` 2.8 vs 6.8, bias deviation ~20× larger). More light on the slave, not less.
5. **Scale-out is no longer blocked** — the boards and cameras arrived. It moved to
   [`PLAN_NETWORK.md`](PLAN_NETWORK.md) and is now a UDP/Ethernet job, not a UART one.

## Phase 5 — Focus display

Show *what is being measured, while it is being measured*. A "Focus:" panel under the progress
card displays the current target in large type for exactly the window its bits are collected in:
the candidate number during scoring, the full draw during combination measurement. Run lengths
are retuned so those windows are the run, not a delay bolted onto it.

**The point is conscious noticing, not reading.** The observer is not meant to decode 6 numbers
in half a second — they are meant to *be present* while those numbers are on screen and the
noise is sampled. That is the deliberate intent, and it flips the usual UI priorities:

- **Salience over legibility.** Large and high-contrast because it must register, not because it
  must be parsed. No need to shrink type to fit, wrap, or scroll.
- **Onset is the payload.** What the observer notices is the *change*. The panel must visibly
  transition at the start of each window — the failure mode is a panel that looks static and
  slides past unnoticed, not one that is hard to read.
- Legibility is therefore not a gate criterion, and 500 ms is not a compromise to be walked back.

This makes the observer part of the measurement window — the original GCP/PEAR protocol, where
the point is precisely that a person attends while the noise is sampled. It changes nothing
statistically (z is normalised by √segments at any run length), but a session run with the Focus
panel is no longer equivalent to an unattended one, so results must record which mode produced
them rather than being pooled later.

**This is explicitly experimental.** The value being tried is whether attention coincident with
sampling shows up in the statistics at all. Build it to be tried and tuned, not to be right
first time — every duration is a constant, and a Focus session is tagged as such.

### Spec

| Phase | Panel shows | Held for | Source |
|-------|-------------|----------|--------|
| Number scoring | the single candidate number | **1000 ms** | `score_and_build_pool()` current `k` |
| Combination measurement | the whole draw (6, or 5 + 2 euro) | **500 ms** | `results[i].nums` / `.euro` |
| Baseline / idle | nothing (panel hidden) | — | — |

- The hold time **is the run**, not a pause around it: size segments-per-run so the measurement
  occupies the window. Padding with a delay would halve the bit rate for nothing.
- The panel updates **before** the run starts and stays put until it ends, so display and bits
  cover the same interval. Anything else defeats the purpose.
- Order stays the per-loop Fisher–Yates permutation, so the observer cannot anticipate the
  next target — and drift immunity is unchanged.

### Timing budget — one loop ≤ ~10 min

Measured: a camera run of 8000 segments takes ≈ 0.47 s master-only, ≈ 0.66 s slave-combined.
Segment counts must therefore be **calibrated against a real run**, not derived on paper, and
the chosen values recorded here.

| | target | ≈ segments | count | ≈ time |
|---|---|---|---|---|
| Scoring run | 1000 ms | ~12–17k | 49 × `SCORE_REPS` (10) = 490 | ~8 min, **loop 0 only** |
| Baseline run | (not shown) | measurement length | 50 | ~25 s |
| Measurement run | 500 ms | ~6–8k | `Runs` cap **1000** | ~8.3 min |

→ loop 0 ≈ 17 min (scoring is one-time), every later loop ≈ **9 min**. Eurojackpot scoring is
62 numbers ≈ 10 min. Suggested UI defaults change to `Runs = 1000`, `Baseline = 50`.

The `Runs` cap already stride-samples across the whole combination space (slot i → combo
⌊i·full/total⌋), so 1000 of 5005 stays spread rather than taking a lexicographic prefix, and
cumulative mode re-measures the same 1000 slots each loop — Stouffer accumulation is unaffected.

### Implementation notes

- **Segments per run become phase-dependent**, not just source-dependent: `SCORE_SEGMENTS` and
  `MEAS_SEGMENTS` alongside the existing `CAM_SEGMENTS`/`TRNG_SEGMENTS`. z stays N(0,1) because
  it is normalised by √segments — but the two nodes must use the **same** count for the same
  run, so the slave has to be told the length rather than assuming it. Today `M` implies one
  fixed length; extend it to carry the segment count. **Coordinate with `PLAN_NETWORK.md`
  Phase C**, which is rewriting that exchange onto UDP anyway — doing both at once avoids
  changing the protocol twice.
- **Serve the focus separately from `/status`.** `/status` is ~2.5 KB and polled at 1 Hz — far
  too fat and too slow for this. Add a small `GET /focus` (~60 bytes) carrying the current
  target plus a monotonic `focus_seq`; `/status` stays at 1 Hz for everything else.
  `focus_seq` is what tells the UI a *new* window started, including when two consecutive draws
  happen to look similar.

### Synchronisation — the part that can quietly invalidate the whole idea

If the observer's attention is supposed to coincide with the sampled bits, then **display
latency and jitter are not cosmetic**. Polling `/focus` at 4 Hz puts up to 250 ms of jitter on a
500 ms window: the numbers could appear halfway through the run they belong to, or after it
ended. The experiment would then be measuring attention against the *wrong* bits, and would look
like a null result no matter what is true.

Options, cheapest first:

1. **Poll `/focus` at 10 Hz** — ~600 B/s, trivial for the ESP. Bounds jitter at ≤100 ms (20 % of
   a 500 ms window). Good enough to start, and the simplest thing that could work.
2. **WebSocket push** on run start (`esp_http_server` supports it —
   `examples/protocols/http_server/ws_echo_server`). Sub-10 ms, no polling. The right answer if
   step 1's jitter turns out to matter.
3. **Scheduled targets**: device sends the *next* target plus a start timestamp, UI syncs a
   local clock and renders on time. Most precise, most machinery — only if 1 and 2 disappoint.

Start with (1), but **measure the jitter rather than assuming it** — see the gate.
- UI: a card below the progress card, title "Focus:", reusing the existing `.num` circle styling
  at a larger size; hidden unless a target is active.

### Open

- Hold times start at **1000 ms scoring / 500 ms draw** and are constants, ideally UI fields.
  500 ms for the draw is a deliberate choice, not a compromise — tune from experience, not from
  a readability argument.
- Flag a Focus session in `/status` and in the CSV export, so attended and unattended runs are
  never pooled later.
- Whether the transition itself should be emphasised (brief scale or highlight at onset) to make
  the change easier to notice peripherally. Try plain first.

### Gate

- Panel updates in lockstep with the measurement: `focus_seq` strictly increasing, no skipped or
  repeated targets over a full loop.
- **Display-to-measurement jitter bounded and actually measured** — not assumed. Log the
  device-side run-start time and the browser-side render time for a few hundred windows and
  report the distribution; ≤100 ms is acceptable at a 500 ms window, and if it is not achieved,
  move to the WebSocket option before running any real session. A misaligned display would make
  the whole experiment read as null regardless of the truth.
- Measured run durations within ±10 % of 1000 ms / 500 ms; calibrated segment counts recorded.
- One measurement loop ≤ 10 min.
- **`loop_sigma` ≈ 1 and `pair_r` ≈ 0 at the new run lengths** — the retune must not disturb the
  statistics, which is the one way this change could do real damage.

Note what this gate deliberately does *not* test: whether the focus has any effect. That is the
experiment, not the acceptance criterion. The gate only establishes that the instrument does
what it claims — right target, right window, undisturbed statistics — so that a null result can
be trusted as a null result.

## Phase 4 — 4-node scale-out — **SUPERSEDED by `PLAN_NETWORK.md`**

**Do not implement the UART-star design below.** The hardware it assumed (2 boards, one
crossover cable already wired) no longer describes the setup: there are now 4× ESP32-P4-ETH,
each with its own OV5647, plus a 4-port PoE switch. With Ethernet on every node the transport
decision was reopened deliberately and settled on **UDP broadcast sync + Ethernet OTA** — see
[`PLAN_NETWORK.md`](PLAN_NETWORK.md), which also covers firmware delivery and retiring USB.

This document remains the contract for the **noise source and the statistics** (Phases 0–3).
`PLAN_NETWORK.md` owns transport, provisioning and firmware delivery, and changes neither what
is measured nor how a z-score is computed.

The original UART-star design is kept below as a record of the reasoning, not as work to do.

- Master: `slaves[]` array {uart_num, tx, rx, ok}; UART2/3 pins chosen from free header
  GPIOs (verify against Waveshare pinout; avoid 14/15 UART1, 31/51/52 ETH MDC/MDIO/RST,
  RMII-fixed pins, 37/38 console). Broadcast `M` to all healthy slaves **before** the local
  measurement, collect replies after, per-slave timeouts as today.
- Generalize PairStats: one (master, slave_i) pair-tracker per slave; publish per-node σ
  and max |r| (flag ⚠ if any |r|·√n > 3).
- UI: badge "N-node • SNR ×√N", per-node health/σ row in the stats line.
- Baseline `B` broadcast + wait-all; abort `A` broadcast (already per-UART).
- **Gate:** 4-node quick session; all pairwise r ≈ 0; combined σ ≈ 1; a node unplugged
  mid-run degrades gracefully to √3 with a UI flag.

## Hardware checklist

Done today: 2× ESP32-P4-ETH (master COM4, slave COM6), 1× OV5647 wired to master.

Before Phase 2: 2nd OV5647 wired to the slave's CSI connector, light-tight capping for
both cameras (cap + tape + opaque box).

Before Phase 4 (not yet acquired):
1. 2 more ESP32-P4-ETH boards + 2 more OV5647 modules.
2. **CSI connector/cable match**: OV5647 modules usually ship RPi-style 15-pin 1.0 mm FFC;
   many P4 boards use 22-pin 0.5 mm — verify, order 15↔22 adapter cables if needed.
3. Confirm the ESP32-P4-ETH board exposes MIPI-CSI at all (else cameras go on whichever
   boards do, roles reassigned — slaves don't need Ethernet).
4. UART wiring: 2 more crossovers (masterTX→slaveRX, masterRX←slaveTX) + common GND star.
5. Power for 4 boards + cameras (USB per board is fine; avoid one weak hub).

## Workflow

Planning/architecture: Fable/Opus (this document is the contract). Implementation: Sonnet,
one phase per session, prompt: *"Implement Phase N of docs/PLAN_4NODE.md — everything you
need is in that file and CLAUDE.md."* Escalate back to Fable only if a phase gate fails
twice or an architectural decision is missing here. Commit at every green gate; master and
slave repos must be committed together when the shared component changes.
