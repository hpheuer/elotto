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

**Proposed objective, quality-first:** among the settings that pass the quality gates, pick the
one with the highest sustained Mbit/s.

- Hard gates (inherited from the original Phase 0 gate, which these cameras have met before):
  `|bias − 0.5| < 1e-3`, `|autocorr lag 1..4| < 0.01`, per-mini-run σ within 1 ± 0.05,
  zero stuck frames, and `mean_pixel_level` below the light-leak threshold.
- Tie-break and only tie-break on rate.

Rationale: the entropy is the instrument. A faster stream that is measurably less uniform buys
granularity at the cost of the thing being measured, and the project has consistently refused
that trade (a stalled camera drops the node rather than substituting the TRNG). **This is a
decision to confirm, not something to infer from the wording.**

Worth knowing while deciding: `CONFIG_ELOTTO_CAM_XOR_FOLD` is on by default and halves the bit
rate to square away the raw LSB bias. If calibration can find a setting whose *raw* bias passes
the gate, the fold could be turned off and the rate would double — a larger win than anything the
exposure sweep alone will produce. Worth testing during the sweep, not assumed.

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

1. **Per-loop cost.** A sweep of K settings × T seconds runs at the start of *every* loop, and a
   loop is currently ~10 min. Ten candidates at 3 s each is 30 s, i.e. ~5 % overhead per loop,
   which is affordable; a fine sweep is not. Suggested shape: full ladder on loop 0, then on later
   loops only re-verify the current setting against the gates and step one notch if it has
   drifted. That honours "at the beginning of each loop" while keeping the cost bounded.
2. **⚠ Re-tuning between loops changes the instrument mid-session.** Loop 1 and loop 2 would be
   measured with different sensor settings. This is a milder version of the thing the abort-on-
   stall policy exists to prevent (never mix entropy sources inside one session, because a
   session-level flag cannot say which runs were affected). It is probably acceptable —
   `studentize()` removes each loop's own offset exactly, so a per-loop change is absorbed the way
   the camera's residual bias already is — but it must be **recorded per loop**, in `LoopStat` and
   in `/loops`, so any effect is attributable to the setting that produced it. Do not ship the
   re-tune without the record.
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

### 1.7 Open questions for the user

1. **Confirm the objective** in §1.3: quality gates first, rate only as tie-break. The alternative
   reading of "maximum random data stream" is rate-first, which would trade away uniformity.
2. **Full sweep every loop, or full on loop 0 and a cheap re-verify afterwards?** (§1.5.1)
3. **Is the XOR-fold in scope?** Turning it off doubles the rate if raw bias passes, and
   calibration is the natural place to find out.

---

## Workflow

Planning/architecture: Fable/Opus — this document is the contract. Implementation: Sonnet, one
task per session. Escalate back if a gate fails twice or a decision above is missing. Commit at
every green gate; the master and slave repos must be committed and flashed together whenever the
shared `components/` or the wire protocol changes.
