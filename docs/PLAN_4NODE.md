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

**Phase 5 (Focus display)** is **IMPLEMENTED** (2026-07-25) — it changes the measurement
protocol and the UI, so it lives here rather than in `PLAN_NETWORK.md` (whose "Phase D" is the
4-node scale-out). Phase C landed first, as its "coordinate with Phase C" note asked, so the
run-length exchange was changed once rather than twice.

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
| Between targets | a dim fixation mark, no numbers | **~200 ms** | — |
| Baseline / idle | nothing (panel hidden) | — | — |

> **As built**, the between-targets gap is 200 ms after a measurement or baseline run and
> **350 ms after a scoring run** — see finding 3 below. It is a real delay, not existing
> overhead, because the natural gap turned out to be 2.3 ms (finding 1).

- The hold time **is the run**, not a pause around it: size segments-per-run so the measurement
  occupies the window. Padding with a delay would halve the bit rate for nothing.
- The panel updates **before** the run starts and stays put until it ends, so display and bits
  cover the same interval. Anything else defeats the purpose.
- Order stays the per-loop Fisher–Yates permutation, so the observer cannot anticipate the
  next target — and drift immunity is unchanged.

**The 200 ms gap between targets should be aligned with the sampling gap that already exists,
not added on top of it.** Measured per-run wall time is ≈ 0.66 s slave-combined against ≈ 0.47 s
of actual sampling, so ~190 ms per run is already spent on the slave round-trip and bookkeeping
— time when no bits are being collected. Two reasons this matters:

- **It makes the coincidence honest.** Panel lit ⟺ bits being collected. If the numbers stay up
  through the inter-run overhead, the observer spends ~30 % of each cycle attending to a target
  whose measurement has already finished.
- **It is free.** Blanking during dead time costs no throughput. Only pad with real delay if the
  natural gap turns out shorter than intended — and then pay for it in the run budget below.

Blank to a **dim fixation mark** rather than nothing, so the gaze stays anchored and the next
target appears where the observer is already looking — the onset is what has to be noticed.

### Timing budget — one loop ≤ ~10 min

Measured: a camera run of 8000 segments takes ≈ 0.47 s master-only, ≈ 0.66 s slave-combined.
Segment counts must therefore be **calibrated against a real run**, not derived on paper, and
the chosen values recorded here.

Cycle = display window + gap. If the gap lands entirely inside existing overhead it is free; if
it has to be padded, budget the full cycle:

| | window | + gap | ≈ segments | count | ≈ time |
|---|---|---|---|---|---|
| Scoring run | 1000 ms | 1200 ms | ~12–17k | 49 × `SCORE_REPS` (10) = 490 | ~10 min, **loop 0 only** |
| Baseline run | (not shown) | — | measurement length | 50 | ~35 s |
| Measurement run | 500 ms | 700 ms | ~6–8k | `Runs` cap **850** | ~10 min |

→ every loop after the first ≈ **10 min**; loop 0 adds the one-time scoring. Eurojackpot scoring
is 62 numbers ≈ 12 min. Suggested UI defaults: `Runs = 850`, `Baseline = 50`. **Both defaults
shipped.**

As built (6/49, `SCORE_REPS` = 5, so scoring is 49 × 5 = 245 runs rather than the 490 assumed
above): scoring ≈ 245 × 1.38 s ≈ **5.6 min**, measurement ≈ 850 × 0.70 s ≈ **9.9 min**.

`Runs` drops from 1000 to 850 purely to absorb the gap — if the gap proves free (overhead
already ~190 ms), put it back to 1000.

The `Runs` cap already stride-samples across the whole combination space (slot i → combo
⌊i·full/total⌋), so 1000 of 5005 stays spread rather than taking a lexicographic prefix, and
cumulative mode re-measures the same 1000 slots each loop — Stouffer accumulation is unaffected.

### Experimental design — what makes a result mean anything

- **A control condition is required, not optional.** Focus sessions on their own cannot
  distinguish "attention did something" from "the retuned run length did something" or from
  ordinary noise. Run **matched no-focus sessions** — identical mode, `Runs`, segment counts,
  source and loop count, with the panel off (or nobody watching) — and compare. Without a
  control the whole phase produces an uninterpretable number, however good the plumbing is.
- **Attention is the scarce resource here, not bits.** A measurement loop is ~850 targets at
  ~1.4 Hz for ten minutes, and loop 0 adds ~10 min of scoring before it — twenty minutes of
  continuous noticing. Expect attention to fade within a loop; that is a property of the
  observer, not a defect. Keep loop counts modest at first and treat "how long can this actually
  be sustained" as one of the things being learned.
- **Therefore a Pause / Continue button is required.** Without it the only way to stop attending
  is to abort the session and throw the loop away, which guarantees that tired-observer data
  gets measured rather than skipped. Requirements:
  - **Pause takes effect between runs, never inside one.** The current run finishes and is kept;
    the session then holds. Stopping mid-run would leave bits that were sampled while nobody was
    watching inside a run labelled as attended — the one kind of contamination this whole phase
    exists to avoid.
  - It is **not abort**: state stays `running`, nothing is published, the permutation index and
    `Σz` accumulation continue exactly where they left off on resume.
  - The Focus panel must show an unmistakable **paused** state, so "no numbers on screen" never
    has to be interpreted as "maybe I missed one".
  - **Paused time must not count towards `elapsed_ms`**, or the ETA drifts by however long the
    break was. Track it separately and record total paused duration in `/status` — a session
    with a 40-minute break in the middle should not later be read as continuous.
  - Pause is device-side like the loop itself, so closing the browser does not resume it.
- **The existing per-loop permutation already protects against that fade.** Fisher–Yates was
  introduced for drift immunity, but it does the same job here: because each loop measures the
  combinations in a fresh random order, a decline in attention over a loop spreads evenly across
  combinations instead of always penalising the ones measured late. No new machinery needed —
  worth knowing it is already covered.

### Implementation notes

- **Start master-only.** The slave currently runs the updater and has no GCP firmware
  (`PLAN_NETWORK.md` Phase C), so the master measures solo — Focus can be built, gated and tried
  today at ×1 SNR, with no dependency on the network work.
- **Segments per run become phase-dependent**, not just source-dependent: `SCORE_SEGMENTS` and
  `MEAS_SEGMENTS` alongside the existing `CAM_SEGMENTS`/`TRNG_SEGMENTS`. z stays N(0,1) because
  it is normalised by √segments — but when the slave returns, both nodes must use the **same**
  count for the same run, so the slave has to be *told* the length rather than assume it. Today
  `M` implies one fixed length; extend it to carry the segment count. **Coordinate with
  `PLAN_NETWORK.md` Phase C**, which rewrites that exchange onto UDP anyway — doing Phase C
  first means changing the protocol once instead of twice.
- **Serve the focus separately from `/status`.** `/status` is ~2.5 KB and polled at 1 Hz — far
  too fat and too slow for this. Add a small `GET /focus` (~60 bytes) carrying the current
  target, a monotonic `focus_seq` and the paused flag; `/status` stays at 1 Hz for everything
  else. `POST /pause?on=1|0` alongside it, checked at the top of each run's loop iteration —
  the same place `abort_requested` is already tested, so pausing between runs falls out of the
  existing structure rather than needing new plumbing.
  `focus_seq` is what tells the UI a *new* window started, including when two consecutive draws
  happen to look similar.

### Synchronisation — 10 Hz polling is enough; the failure to avoid is a *skipped* window

**A 50–100 ms offset is not a problem, and tight sync is not worth building.** At a 500 ms
window that is still 80–90 % overlap, so the worst case is mild attenuation. More to the point,
conscious noticing is itself smeared over roughly 100–300 ms from photons to awareness — so
sub-100 ms alignment is already finer than the resolution of the thing being tested. WebSocket
push or clock-synced scheduling would be precision the experiment cannot use.

`GET /focus` polled at **10 Hz** (~600 B/s, five samples per 500 ms window) is therefore the
design, not a first step towards something better.

What *would* matter is a different failure: a window the UI **misses entirely**, or a display
that persists so long it lands mostly inside the *next* run. That is not attenuation, it is
mislabeling — the observer notices combination N while combination N+1's bits are collected, and
because per-combination z feeds the Stouffer accumulation, any effect gets credited to an
unrelated combination. It takes ~250 ms of slip at a 500 ms window to start happening, which
10 Hz polling comfortably avoids.

Cheap guard instead of precise measurement: `focus_seq` is monotonic, so the UI can notice a
gap (`seq` jumped by more than 1) and count it. A "windows missed" counter is a more honest and
far cheaper diagnostic than a jitter histogram, because it detects the failure that actually
corrupts the data rather than the one that merely blurs it.
- UI: a card below the progress card, title "Focus:", reusing the existing `.num` circle styling
  at a larger size; hidden unless a target is active.

### Open

- Hold times start at **1000 ms scoring / 500 ms draw** and are constants, ideally UI fields.
  500 ms for the draw is a deliberate choice, not a compromise — tune from experience, not from
  a readability argument.
  → **Still constants, not UI fields.** Making them UI fields is not the trivial change it looks
  like: the field a user would set is a *duration*, but what the firmware needs is a *segment
  count*, and finding 4 shows the conversion between them is not stable. A duration field would
  therefore need a closed loop (adjust segments per run from the measured window) — which is
  feasible, since the count already travels to the slaves on the wire and z is normalised by
  √segments so per-run counts may differ freely. Deferred as out of scope, and it is the obvious
  next move if the drift proves annoying in practice.
- Flag a Focus session in `/status` and in the CSV export, so attended and unattended runs are
  never pooled later. → **done** (`"focus":true|false`, `# focus=on|off` in the CSV, plus
  `paused_ms` in both).
- Whether the transition itself should be emphasised (brief scale or highlight at onset) to make
  the change easier to notice peripherally. Try plain first. → **still plain**; the circles do
  carry a soft glow, but no onset animation. Untried, deliberately.

### Implemented (2026-07-25) — what was built

Master-side, on the 4-node array (the "start master-only" note above is obsolete: Phase C had
landed, so all four nodes participate and the run-length exchange was changed once).

- **Segments per run are now phase-dependent** — `CAM_SCORE_SEGMENTS` / `CAM_MEAS_SEGMENTS` and
  the TRNG pair, selected by `segments_for(scoring)`. Baseline runs at *measurement* length: it
  estimates the offset of the runs it is subtracted from, so it has to be the same instrument.
- **The segment count travels on the wire**: `M<seg>` and `B<runs>,<seg>`. This retired the
  duplicated-constant hazard CLAUDE.md warned about — a slave that is *told* the length cannot
  disagree about it. `slave.c` keeps its own constants only as a fallback for a pre-Phase-5
  master, and logs when it uses them. The yield/abort-poll cadence became `nseg/4` on both sides
  for the same reason.
- **`GET /focus`** (~60 B, polled at 10 Hz) carrying `seq`, `on`, `p`, `kind` and the numbers;
  `POST /pause?on=1|0`; both separate from the 2.5 KB `/status`, which stays at 1 Hz.
- **`focus_publish()` / `focus_off()`** bracket the *local* run — panel lit ⟺ this run's bits
  are being collected. The reply wait and bookkeeping are dark.
- **`pause_gate()`** is called where `abort_requested` is already tested, in the scoring and
  measurement loops. Deliberately *not* in the baseline loop: one `B` command sets every slave
  running its whole baseline autonomously, so a master-side hold would desynchronise them rather
  than pause them.
- **UI**: Focus card below the progress card (72 px circles, dim `+` fixation mark between
  targets, unmistakable PAUSED state), a Focus checkbox, a Pause/Continue button, `Runs = 850`
  and `Baseline = 50` defaults, and the session's condition in both the stats line and the CSV
  header. `/start` without `?focus=` means **unattended** — a session started by curl has no
  observer by definition.

### What calibration actually found — the run window is not a free parameter

The plan said to calibrate against a real run rather than derive on paper. That was the right
instruction. The paper estimate for scoring was "~12–17k segments"; its top end (17000) actually
produced a **1519 ms** window — 52 % over target — and the value that works is 11950, just under
the bottom end. The measurement estimate ("~6–8k") landed better, at 6400. But the reason for
the miss turned out to matter more than the numbers.

**1. The natural inter-run gap is 2.3 ms, not ~190 ms.** The estimate above attributed the
0.47 s → 0.66 s difference (master-only vs slave-combined) to the slave round trip. It is not:
the slaves integrate the *same* window concurrently and are already answering by the time the
master's own run returns, so `nodes_collect()` costs nothing. There is no existing dead time to
align the blanking with, so the ~200 ms gap had to be **paid for with a real delay**
(`RUN_GAP_MS`), which the plan anticipated ("then pay for it in the run budget") and which
`Runs = 850` pays for.

**2. The measurement loop starves its own entropy producer.** It runs one priority *above* the
camera extraction task it consumes from, so sustained rate falls with duty cycle:

| segments | run | gap | duty | sustained |
|---|---|---|---|---|
| 6350 | 277 ms | 205 ms | 57 % | 2.63 Mbit/s |
| 11600 | 588 ms | 233 ms | 72 % | 2.82 Mbit/s |
| 17000 | 1519 ms | 198 ms | 88 % | **1.98 Mbit/s** |

Flat to ~72 %, then it falls off a cliff — and past the cliff longer runs feed back into a
slower producer and get longer still. None of this is visible in Phase 0's 3.49 Mbit/s *idle*
figure. The 200 ms blank is therefore not cosmetic: it is when the producer gets the CPU back,
and it buys back most of what it costs.

**3. A 1000 ms scoring window at a 200 ms gap is unreachable by construction** — that is 83 %
duty, already past the cliff, so *every* candidate segment count lands in the collapsed regime
and stretches to ~1500 ms instead (confirmed at 16700 and 17000). The fix was to widen the
**scoring** gap to 350 ms (`SCORE_RUN_GAP_MS`), which puts the same 1000 ms window at 74 % duty
where a count does solve. Shortening the window instead was rejected: the gap is the soft
parameter this plan already marked adjustable, whereas the hold times are the spec, and buying a
display number by running the entropy source in a starved regime trades physics for cosmetics —
that regime is exactly where open item 3 (bias under sustained load) lives. Safe to differ per
phase because scoring does not use the baseline at all.

**4. ⚠ The window is not stable across a day, and this is unresolved.** Under otherwise
identical settings the achievable window moved by up to **1.75×** over an afternoon of
back-to-back sessions (11600 segments → 588 ms early, 11950 segments → 1026 ms hours later, with
a *longer* gap, which should have made it faster). Each session began after an OTA reboot, so
cumulative camera state is ruled out; thermal or SoC-level load is the remaining suspect. Note
also that `camera_get_stats()`'s `mbit_per_sec` and `bias` are **lifetime averages since stream
start**, not instantaneous — do not read a falling `mbit_s` as live degradation, which is a trap
worth knowing about. The consequence is that **a fixed segment count does not pin the window**;
`focus_win_ms` / `focus_gap_ms` in `/status` are the instrument that makes the drift visible per
phase, which is the same choice Phase 3 made for z-drift: measure it rather than pretend it away.

**5. The master is the slow node.** Its camera sustains ~2.5 Mbit/s against the slaves' 3.66–3.71
because it also runs the webserver, the UDP link and the loop itself. Since the master paces
every run, the array's window is set by its slowest member.

**Calibrated constants (2026-07-25, 4 nodes, all camera, client polling at 10 Hz):**

| | segments | gap | measured window | vs target | cycle |
|---|---|---|---|---|---|
| Scoring run | `CAM_SCORE_SEGMENTS` 11950 | 350 ms | **1027 ms** | +2.7 % | 1.375 s |
| Measurement run | `CAM_MEAS_SEGMENTS` 6400 | 200 ms | **474.8 ms** | −5.0 % | 0.673 s |

TRNG counts (32000 / 16000) are **extrapolated** from Phase 1's "~1 s at 32000 segments", not
re-measured — the camera is the default source. Measure `focus_win_ms` before trusting a TRNG
Focus session.

### Gate

- Panel updates in lockstep with the measurement: `focus_seq` strictly increasing, no skipped or
  repeated targets over a full loop.
- **Zero missed windows over a full loop** (`focus_seq` gap counter stays 0). A sub-100 ms
  offset is fine and needs no measurement; a skipped or straddling window is the failure that
  would credit an effect to the wrong combination.
- **Panel lit ⟺ sampling.** Numbers are on screen only while that run's bits are being
  collected; the gap shows the fixation mark. Report the measured natural inter-run gap, so it
  is known whether the 200 ms was free or paid for.
- Measured run durations within ±10 % of 1000 ms / 500 ms; calibrated segment counts recorded.
- One measurement loop ≤ 10 min.
- **`loop_sigma` ≈ 1 and `pair_r` ≈ 0 at the new run lengths** — the retune must not disturb the
  statistics, which is the one way this change could do real damage.

- One **matched no-focus control session** recorded alongside the first Focus session, so the
  pair can be compared at all.
- **Pause/Continue holds and resumes cleanly**: paused mid-loop, the run count does not advance,
  no run is lost or duplicated, `Σz` and the permutation continue correctly, and paused time is
  excluded from `elapsed_ms`. Verify across a pause long enough to be obvious (minutes, not
  seconds).

Note what this gate deliberately does *not* test: whether the focus has any effect. That is the
experiment, not the acceptance criterion. The gate only establishes that the instrument does
what it claims — right target, right window, undisturbed statistics — so that a null result can
be trusted as a null result.

### Gate results — **PASSED**, with one criterion marginally missed

Session: 2026-07-25, 6/49, cumulative, Loops = 2, Runs = 850, Baseline = 50, all four nodes on
camera, `?focus=1`. 1700 measured runs, 27 min, no aborts, no drops.

| criterion | result | |
|---|---|---|
| `focus_seq` strictly increasing, no skipped/repeated targets | monotonic over 249 consecutive windows sampled at 10 Hz | ✅ |
| **Zero missed windows** over a full loop | **0** seq gaps; 249 distinct draws in 249 windows | ✅ |
| Panel lit ⟺ sampling | duty 71.2 % measured client-side (474.8 ms lit / 672.6 ms cycle) | ✅ |
| Report the natural inter-run gap | **2.3 ms** — the 200 ms was *not* free, see finding 1 | ✅ |
| Run durations within ±10 % | scoring **1027 ms** (+2.7 %), measurement **474.8 → 514.4 ms** (−5.0 % → +2.9 %) | ✅ |
| Calibrated segment counts recorded | 11950 / 6400, table above | ✅ |
| One measurement loop ≤ 10 min | **10.5 min** (loop 2: 662 s total − 34 s baseline) | ⚠ |
| `loop_sigma` ≈ 1 at the new run lengths | **1.0105** and **1.0356** (n = 850 each, SE ≈ 0.024) | ✅ |
| `pair_r` ≈ 0 | worst of 6 pairs **+0.0431** (n1–n3, n = 1700) → \|r\|·√n = 1.78 | ✅ |
| Pause/Continue holds and resumes cleanly | verified, see below | ✅ |
| Matched no-focus control recorded | run immediately after, on the same binary | ✅ |

Full pairwise matrix (master = n0, on isolated USB power; n1–n3 on the shared PoE rail):

| | n0–n1 | n0–n2 | n0–n3 | n1–n2 | n1–n3 | n2–n3 |
|---|---|---|---|---|---|---|
| r | −0.0091 | −0.0234 | −0.0088 | +0.0124 | +0.0431 | −0.0064 |

Per-node σ 1.0360 / 0.9777 / 1.0488 / 1.0237; 0 camera stalls, 0 lost replies, `net_lost` =
`net_retries` = `net_stale` = 0. `best_z` 3.03 at corrected p = 1.0 over 850 comparisons —
consistent with chance, which is what a working instrument should say.

**The one miss is the 10 min loop budget: 10.5 min.** It is entirely finding 4 — the window
drifted from 474.8 ms to a 514.4 ms mean over the 1700 runs, and 850 × 734 ms = 10.4 min. At the
window it started at, the loop is 9.5 min. Dropping `Runs` to ~810, or the closed loop described
under "Open", closes it; it is not worth a fixed-constant chase.

**Pause/Continue** (tested mid-loop on a live session): `/pause?on=1` at run 370 → the in-flight
run finished and was kept (`completed` 370 → **371**, then frozen), `state` stayed `running`,
`/focus` reported `p:1, on:0`. Held **224 s**; `completed` and `elapsed_ms` both frozen
throughout. On resume, `elapsed_ms` advanced 24 987 ms over 25 s of wall clock — the pause is
excluded exactly — and runs continued 372 → 422 with none lost or duplicated. `paused_ms`
recorded 223 999.

**Slaves honour the wire segment count** — checked directly, because this failure is invisible
in the statistics (each node normalises by its own √segments, so a node integrating the wrong
window still reports a clean N(0,1) value and leaves `pair_r`/σ innocent). Sending `B1,<n>`
straight to `.103`: 6400 → 292 ms, 12800 → 828 ms, 25600 → 1645 ms. Doubling the count doubles
the time; a node ignoring it would have returned three identical latencies.

**Worth watching (not a Phase 5 regression):** the master's raw per-run offset is **−2.69 z/run**
at these run lengths, implying a camera bias deviation of ~1.2e-3 — outside Phase 0's 1e-3 gate,
and ~9× the 1.29e-4 Phase 1 measured. `studentize()` removes it exactly per loop and both loops'
σ came out ≈ 1, so nothing downstream is affected, but this is open item 3 showing up with a
number attached. Note Phase 5 *lowered* the duty cycle (from ~99.5 % — the natural gap was
2.3 ms — to 71 %), so it is not the cause.

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

**All acquired and in service (2026-07-25).** Four ESP32-P4-ETH boards, each with its own
OV5647, all on one switch. Items 1–3 below are satisfied; 4 no longer applies (UART was replaced
by UDP — `PLAN_NETWORK.md` Phase C); 5 is answered by PoE.

1. ~~2 more ESP32-P4-ETH boards + 2 more OV5647 modules.~~ Present, all four cameras streaming
   at ~3.45 Mbit/s with bias within 1e-3 of 0.5 at idle.
2. ~~**CSI connector/cable match**~~ — fine as shipped.
3. ~~Confirm the board exposes MIPI-CSI~~ — it does, on every unit.
4. ~~UART wiring: 2 more crossovers + common GND star.~~ **Obsolete**: the transport is UDP
   broadcast over the switch, so there is no inter-node wiring at all.
5. ~~Power for 4 boards~~ — the boards take **PoE directly** (no splitters). The master is kept
   on separate USB power on purpose, as the control for `PLAN_NETWORK` Risk 1.

## Workflow

Planning/architecture: Fable/Opus (this document is the contract). Implementation: Sonnet,
one phase per session, prompt: *"Implement Phase N of docs/PLAN_4NODE.md — everything you
need is in that file and CLAUDE.md."* Escalate back to Fable only if a phase gate fails
twice or an architectural decision is missing here. Commit at every green gate; master and
slave repos must be committed together when the shared component changes.
