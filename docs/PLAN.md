# PLAN: elotto — the live contract

**Task 1 (per-loop camera calibration) is COMPLETE and its control passed.** What follows is
everything still open or recently measured. The design work and the closed findings that got here
— **§1.1 through §1.14** — are in [`PLAN_HISTORY.md`](PLAN_HISTORY.md); section numbering runs
continuously across the two files so existing citations keep resolving.

**Start here:** `CLAUDE.md`'s "Where things stand" section is the one-screen summary of the state
of the rig and the open threads in priority order. This document is the detail behind it.

⚠ `PLAN_4NODE.md` and `PLAN_NETWORK.md` were deleted at the user's request and are in git history,
last present at **`8e134e5`** (`git show 8e134e5:docs/PLAN_4NODE.md`). Source comments still cite
them ("PLAN_4NODE Phase 3", "PLAN_NETWORK §4"); those citations are historical and resolve to git
history, not to a file on disk. They were left alone deliberately.

---

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

### 1.16 Why .145 (slave1) misbehaves — and why more light is the wrong fix (2026-07-27)

`.145` had been the outlier twice: σ **1.1884** across a 6644-run attended session, and σ **2.2673**
in §1.15 arm B loop 5. The obvious hypothesis was under-illumination — it ran exposure 32 where
the master ran 8, and its light-per-unit-exposure was the lowest of the four (1.53 vs 2.1–3.1).

**The slaves' sweeps were being computed, stored in PSRAM, and thrown away unread** — only the
chosen rung travels on the wire, and the slave served just `/` and `/diag`. Added `GET /calibrate`
to the slave, with the serialiser moved into `elotto_camera` so both firmwares emit one shape.
This is what made the rest of this section possible; without it a per-node optical fault is not
diagnosable at all.

**Each node has its own breakdown point, and they differ by nearly 2×:**

| node | last GOOD rung | `mean_px` | σ | first BAD rung | `mean_px` | σ |
|---|---|---|---|---|---|---|
| master | 16 | 58.23 | 0.9579 | 32 | 104.65 | 1.0394 *(light gate, not quality)* |
| .103 | 16 | 37.77 | 0.9819 | 32 | 63.38 | **1.8948** |
| **.145** | 16 | 33.81 | 1.0299 | 32 | **60.31** | **1.3503** |
| .155 | **32** | **67.62** | **1.0207** | 64 | 107.23 | 3.0267 |

**More light would make `.145` worse, not better.** It is mildly dimmer than `.155` at every
matched rung (~5–10 %), so that part of the hypothesis was right — but it *breaks down at less
light*: σ 1.35 at `mean_px` 60.3, where `.155` is clean at 67.6. Its usable ceiling is genuinely
lower. Adding light cannot raise a ceiling; calibration would simply retreat to a shorter exposure
and land back where it already is. **And there is nothing to gain by trying: the bit rate is
CPU-bound** (3.295 vs 3.359 Mbit/s across its whole ladder), so being confined to exposure 16
costs nothing at all.

**The likely mechanism for the intermittency: exposure 32 sits exactly on `.145`'s edge.** It has
been *selected* repeatedly — exp 32 in the 6644-run session (σ 1.1884), exp 32 throughout §1.15
arm A, and inherited as exp 32 for all six loops of arm B, which is the arm that produced the
σ 2.2673 loop. Today the same rung failed the gates outright and calibration chose **16**
(bias +5.0e-6, σ 1.0299 — its best measured state). A rung that passes some sweeps and fails
others is precisely a node that is fine most of the time and occasionally not.

⚠ Not established: this predicts arm A should have been protected by re-selecting every loop, yet
arm A also chose 32 each loop and stayed clean. So the edge is real but the mechanism linking it to
the bad loops is inferred, not shown.

**This is the first concrete support for §1.15's "calibration protects the tail" reading**, and it
suggests the mechanism: per-loop calibration gives a marginal node a fresh chance each loop, where
`cal=0` locks in whatever it entered with for the whole session. Arm B entered on 32 and stayed
there. That remains a hypothesis — testing it needs a count of bad loops over many more loops, not
a comparison of mean σ.

**No action taken on the hardware, and none recommended.** `.145` is correct at exposure 16, the
array passed every gate in §1.15, and one node with a lower ceiling costs a slice of the √n gain,
not correctness. ⚠ Note also that the raised `CAL_MAX_MEAN_PX` of 100 is now *binding on the
master*: its exposure-32 rung fails on light at 104.65 while its σ there is a perfectly good 1.0394.

### 1.17 A 200-loop session, the σ-margin fix, and a failing LED (2026-07-27, evening)

**The run.** 200 loops × 63 combinations, Eurojackpot, attended, 6.46 h. First use of the
attended pool confirmation: `pool_auto = 0`, pool cut to 7 main + 3 bonus = 63 combinations.
Operationally spotless — `net 0/0/0`, 4/4 nodes throughout, no faults, stalls or reboots.

**Result: null.** best |z| **2.96** over 63 comparisons, Bonferroni **p = 0.194**. The expected
largest |z| under chance for 63 comparisons is ≈2.7–2.9. Nothing here.

**Four instrument gates failed**, over the 128 loops `LOOP_HIST` stores:

| gate | result |
|---|---|
| mean σ within 2 SE of 1 | ❌ 1.0343 ± 0.0135 = **2.53 SE** high |
| loop-to-loop spread | ❌ SD **0.1531** vs **0.0891** expected at n = 63 |
| worst \|r\|·√n < 3 | ❌ **3.31** (.103↔.155) |
| drift \|t\| < 3 | ❌ **t = −3.86** |

Excess variance is real: √(0.1531² − 0.0891²) = **0.125**.

**It is two nodes, and the other two are textbook clean:**

| node | mean σ | SD | max |
|---|---|---|---|
| master | 0.9889 | **0.0973** | 1.2532 |
| .155 | 0.9907 | **0.0850** | 1.2176 |
| .103 | 1.0669 | **0.2435** | 2.4282 |
| .145 | 1.0294 | **0.2126** | 3.0082 |

Expected sampling SD at n = 63 is 0.0891; master and .155 sit exactly on it. The instrument and
the refactored code are sound.

**§1.16's rung hypothesis is confirmed outright for `.145`:**

| `.145` on | loops | mean σ | SD | max |
|---|---|---|---|---|
| exp **16** (good rung) | 36 | 0.9886 | **0.0821** | 1.145 |
| exp **32** (bad rung) | 91 | 1.0454 | **0.2454** | **3.008** |

On its good rung it is indistinguishable from a healthy node. Every excursion came from exp 32.

⚠ **This REFUTES "per-loop calibration protects the tail"** — the reading §1.15 and §1.16 found
plausible. Calibration put `.145` on its *bad* rung in **91 of 127 loops (72 %)**. It was not
protecting; it was selecting the fault. Mechanism: the rule minimised |bias − 0.5| among rungs
passing the gates, `.145`'s exp-32 sits *on* the σ gate so it passes some sweeps, and when it did
its bias often measured best. **The rule optimised the quantity that studentization already
removes, and ignored the one that costs SNR.**

**Fix — σ margin (`camera.c`).** A candidate must now clear the σ gate with margin
(|σ − 1| ≤ `CAL_SIGMA_TOL`/2 = 0.025) to be *selectable*; among those, lowest |bias − 0.5| still
wins. Falls back to the bare gate if nothing qualifies. Replayed against all four measured
ladders before flashing: master/.103/.155 keep exactly the rung they had, only `.145` moves.
Verified live afterwards — on the confirming sweep `.145`'s exp 32 **passed the bare gate and was
excluded by the margin**, exactly as designed.

⚠ **The fix is partial and known to be so.** On that same sweep `.145`'s exp **64** cleared the
margin (σ 1.0102) when a ladder nine hours earlier had it at σ 3.0750. The sweep's σ simply does
not always predict the session's, and no selection rule reading that sweep can fully compensate.
A feedback path — the master excluding a node's current exposure after an observed bad loop —
would address the real problem and is not implemented.

### ⚠ `.145`'s ILLUMINATION IS FAILING — physical, unresolved

Comparing matched exposures nine hours apart (12:50 → 22:20), same enclosure, same everything:

| node | ratio |
|---|---|
| .103 | 0.97× |
| .155 | 0.92× |
| **master** | **0.75×** |
| **.145** | **0.35×** |

`.145` at exposure 16 fell `mean_px` **33.81 → 11.61**. The ratio holds across every rung, so it
is genuine light loss, not measurement noise. §1.13's junction-warming figure is ~12 %; this is
65 %. **This is a hardware fault in progress, not drift.**

Note the pattern: the two nodes that fell are the master and `.145` — **the first two to be
lit** (§1.13). Worth checking whether they share a supply, and whether an LED, a solder joint or
a mount is failing. Not diagnosable from software.

Consequence for the analysis above: `.145`'s ceiling result in §1.16 still holds (it is clean to
`mean_px` ≈ 34 and broken by ≈ 46–60 in *both* measurements, where .155 is clean at 63–68), but any
comparison of absolute `mean_px` across today's sessions is confounded by falling light, and the
200-loop session ran while it was falling.

---

### 1.18 Calibration is now time-triggered, and the observer gate holds 1 s (2026-07-29)

Two operator-facing changes, both requested by the user, neither touching the statistics.

**1. The exposure sweep is triggered by wall-clock age, not by the loop boundary.**
`calibrate_all()` skips the sweep while the last one is younger than `g_status.cal_interval_ms`
(default **300 000 ms = 5 min**, `POST /start?calint=<ms>`, 0 = the old sweep-every-loop rule).

The loop was only ever a proxy for "about ten minutes have passed". It stops being one as soon as
a loop is short — a Runs cap, a shrunk pool after `/pool`, or a 6-of-49 run can finish a loop in
minutes or seconds, at which point a **~24 s** sweep is most of the loop and re-derives an
operating point that has had no time to drift. What the sweep corrects is thermal, so its natural
clock is the wall clock.

At the default a full ~10 min loop still sweeps every loop, so **§1.14/§1.15's measured condition
is unchanged** and nothing needs re-running. A comparison against those arms must be started with
`?calint=0` if loops are short.

Recorded, because a per-loop change nobody logged is indistinguishable from drift: `LoopStat.cal_ms`
is **0 on a loop that skipped the sweep** (`/loops` serialises it), `/status` publishes
`cal_interval_ms` and `cal_did_sweep`. The `cam_exp/gain/fold/bias` fields still carry the setting
in force, which on a skipped loop is the one carried over — the operating point that loop measured
at, which is the fact the analysis needs.

⚠ **`camera_get_stats()` is reset by the sweep**, so on a skipped loop `mbit_s`/`bias` in `/status`
and `/loops` span *several* loops instead of one. Same caveat as `?cal=0`, now intermittent.

**2. One second of dark between the Start button and the first number.** `PHASE_READY` releases,
the phase moves to `PHASE_SCORING` (before the delay — a `/status` poll that still saw `ready`
re-raised the overlay), the panel is blank for `READY_SETTLE_MS`, then the first target appears.
Pressing the button is itself an act of attention, and the first number's *onset* is what the
observer is meant to notice; without the gap the first number of the pass is measured while
attention is still on the click. The button now carries the instruction — "Focus and concentrate on
the numbers only" — because it is the last thing read before the pass begins.

Both are loop-0-and-attended-only (the gate rides on `?confirm=1`), so unattended and scripted
sessions are bit-identical to before except for the sweep interval.

### 1.19 Ranking default changed to "keep the most extreme" (2026-07-30)

**User decision, on a GCP reading of what a loop is for.** The operator saw a combination published
at −3 in one loop and −2 later, and expected the −3 to be retained. Under the old default
(CUMULATIVE) it was not: the published value is Σz/√k, so a −3.0 followed by an ordinary +0.5
becomes −2.5/√2 = **−1.77** and leaves the list. That was the mode working as designed.

The new default `RANK_EXTREME` keeps the **high-water and low-water mark per fixed combination**.
The pool still locks after loop 0 — that matters, because "measured lower in another loop" only
means anything if the *same* combination is re-measured, which PEAK (which re-scores the pool every
loop) does not guarantee. So this is a third mode, not either existing one. `?rank=0|1|2` selects
peak / cumulative / extreme; the 0 and 1 meanings are unchanged, only the absent default moved.

Highs and lows are held in two arrays rather than one signed extreme: a combination that reads
+3.1 early and −3.4 later would otherwise silently lose the +3.1 the operator was promised.
`s_zmin[]` lives in PSRAM for the usual reason (a second 64 KB `.bss` array fails the link).

⚠ **The statistical cost is real and is handled, not hidden.** A high-water mark is the maximum of
k independent draws, so under the null it *increases* with every loop — the headline Z gets bigger
whether or not anything is there. `comparisons` therefore scales as `runs_total × k`, the same rule
PEAK uses, so the corrected p does not improve merely by running longer. **The Bonferroni line is
the number to read; the raw Z is not interpretable on its own in this mode.** Note the asymmetry
this creates against CUMULATIVE, where a genuine per-run effect d grows as d√k while noise stays
N(0,1): EXTREME shows you the largest excursion, CUMULATIVE tests whether excursions accumulate.
They answer different questions and a session run in one is not comparable with one run in the other.

Verified end-to-end on hardware: 2 loops × 20 runs, `rank=max` in `/status`, best_z **1.8082 →
1.9908** (never decreasing, which CUMULATIVE does not guarantee) and comparisons **20 → 40**.
Both coverage sets populate, five entries each.

**`RANK_EXTREME_RAW` (`?rank=3`) was added the same day**, on request, to see what studentization
had been doing: identical to EXTREME except that `studentize()` measures the loop's mean and σ but
does not rewrite `results[]`. `loop_sigma` and the drift regression survive — only the rewrite is
optional — so the diagnostics still work. The verification run made the difference visible: loop
means of **+0.4551** and **−0.6869** stayed in the published numbers, giving a high list of
1.245…2.654 against a low list of −1.386…−2.305, asymmetric by exactly the loop offset.
⚠ Its Z is not N(0,1), so the corrected p is uncalibrated (the results line says so); the master's
baseline subtraction stops being inert; and `loop_sigma` becomes the scale rather than a
diagnostic. Raw and studentized sessions are not comparable.

### 1.20 Scoring becomes ONE long window per number (2026-07-30)

Phase 0 now measures each candidate number with **one continuous ~3.4 s run** instead of one ~1 s
run. `SCORE_SEGMENTS` = 3 × `CAM_SEGMENTS`; the count already travelled on the wire and the slave
already accepted anything in [SEG_MIN, SEG_MAX], so **no slave change and no slave reflash**.

The intent is the observer's, not the statistician's: ~1 s is barely time to arrive at a number.
The route there is worth recording because two intermediate forms were rejected on the same
ground. Three short reps in place were tried first and immediately reintroduced the problem that
removed five reps in Phase 5 — a repeated target has no onset, and onset is the payload. One long
run gives **identical arithmetic** (a 3× run is Σdev/√(3N), exactly the Stouffer combination of
three 1× runs, per-number SE ≈ 0.29 at four nodes) while keeping one onset per number.

**The gap had to scale with it**, and this was the real risk: duty cycle, not window length, is
what starves the extraction task, and 3.4 s behind the ordinary 350 ms blank is ~90 % — past the
cliff where the achievable window stretches instead of obeying the count. `SCORE_GAP_MS` = 1000
holds ~77 %. A phase-specific gap does not break the uniform-gap rule: that rule exists so
attended and unattended sessions differ only in the display, and so the baseline matches the runs
it is subtracted from — the baseline feeds Phase 2, and scoring has no baseline subtraction.

⚠ **`LINK_MEAS_MS`'s flat 4000 ms was a latent trap**, not a tuning value. Headroom for a 1 s run,
it is a *deadline* for a 3.4 s one: every slave would still be measuring when it expired, all four
would look silent, and `NODE_MISS_LIMIT` would drop them — leaving a solo-master session that
still looked healthy. It is now `LINK_MEAS_MS_FOR(nseg)`, reproducing the old window at the
measurement length and giving ~10.8 s at the scoring length. Deliberately generous by user
decision: a late drop costs nothing, a false drop costs an unnoticed √(k−1) arm.

Measured on all four nodes: scoring window **3370 ms, spread ±1.5 ms over 62 numbers**, gap
1010 ms, duty 76.9 %, sustained rate **3.37–3.38 Mbit/s** (idle 3.49, collapsed regime 2.68), zero
stalls, zero stuck frames, `net_retries/lost/stale` all **0**, nodes 4/4 throughout. Phase 2 was
unaffected: 1031.5 ms / 355.9 ms, matching its pre-change figures. The 12 % overshoot on the 3 s
target was left as-is (user decision) — it is stable, clear of the starvation regime, and z is
normalised by √segments either way.

---

## 2 v3.0 — the single-pass session (specified AND implemented 2026-08-02)

**Status: flashed to the master and smoke-tested the same day** (commit follows this edit).
Verified live: 400 on `?loops=/?runs=/?rank=`, the full calibrate→baseline→scoring→measuring
flow at 4/4 nodes, single-pass random order (items 6570/4439/7334/7846/6319 of 7920), window
3367.8 ms / gap 1017.6 ms, `/results.csv` streaming the compact prefix with raw z + block
column, abort publishing partials with `comparisons = items_done`, `net 0/0/0`. **Not yet
exercised** (needs wall time, not different code): a mid-pass 15-min block insertion and the
final `close_block()` — both run the same open-insertion path that did execute. First observed
raw-z consequence, as §2.3 predicted: pass_mean sat at −2.26 (the array's common offset, no
longer subtracted), so read extremes against the studentized view or the drift line.

**The core rule generalises: no Focus item is ever measured twice — now in Phase 2 as well.**
Phase 0 already obeys it (one long window per number, §1.20). v3.0 makes the combination pass
obey it too, and everything below follows from that one decision. This is a **major version**:
a v3 session must never be pooled with any v2.x session (window 3× longer, no studentization,
no baseline subtraction — three incompatibilities, any one of which is disqualifying).

### 2.1 Session shape

Calibrate + baseline → observer gate (`/ready`) → Phase 0 scoring (unchanged) → pool
confirmation (unchanged) → **ONE pass over every combination in the confirmed pool, each
measured exactly once**, in a fresh Fisher–Yates random order. Then done. **No loops, no loop
counter, no `Runs` cap.** The progress bar's 100 % is the full combination count; beside it an
**items counter** (`items_done / full_combos`) replaces the loop badge. The two attended gates
are **kept** — they are the protocol, not the ranking.

- **Window/gap: `SCORE_SEGMENTS` (~3370 ms) and `SCORE_GAP_MS` (1010 ms) for Phase 2 too.**
  One cycle ≈ 4.38 s. The duty-cycle reasoning of §1.20 already covers this shape; the segment
  count travels on the wire, so this is a master-only change — no slave reflash.
- **Session length**: Eurojackpot 7920 items ≈ 9.6 h measuring (~10.2 h with insertions);
  6-of-49 5005 items ≈ 6.1 h (~6.5 h). **Attended by assumption** (user, 2026-08-02): the
  observer peeks, lets the rest run subconscious, and uses **Pause** (clock stops, excluded
  from `elapsed_ms` as today) or **Abort** (partial results published from the measured
  prefix — `compact_partial()` already does this) at their own judgment. No time budget.
- **Cap**: `NUM_RUNS` stays **8000**. Both pools already fit (7920 / 5005); the pool
  constants don't change. Repeats across *sessions* are the user's choice; nothing persists.

### 2.2 Blocks replace loops as the statistics unit

**Every ~15 min** (default; `?calint=` keeps its meaning with the new default,
`CAL_INTERVAL_DEFAULT_MS` 5 → 15 min) the pass parks and runs **sweep + baseline together**,
then resumes. That boundary closes a **block**, which inherits everything that was per-loop:
`record_loop()` → per-block row in `/loops` (endpoint name kept), `drift_add()` on the block's
baseline mean, `pairs_fold_loop()` for the per-block-centered pairwise matrix. Eurojackpot:
~205 items/block, ~38 blocks — comfortably past `DRIFT_MIN_LOOPS` = 6, and *finer* drift
resolution than the old ~10-min loop, at ~6 % overhead (~54 s per insertion: 10 baseline runs
at measurement length, 3370 + 1010 ms each, plus the sweep).

- Baseline runs at the **new** measurement length — same-instrument rule (§ Phase 1 comment
  in sensor.c) — and is **drift reference ONLY**: the `zm = zraw − baseline_mean` subtraction
  is **removed** (it was inert under studentization; raw-by-default would have made it a live,
  master-only asymmetry, the §1.19 RAW trap).

### 2.3 z, ranking, results

- **Stored z is always RAW** (the combined Σz/√k, nothing subtracted, nothing rescaled).
  `RANK_*`, the combobox, `s_zsum`/`s_zmin`, `accum_reset()`, `publish_cumulative()`,
  `publish_extreme()`, `absorb_loop()`, Stouffer — all deleted. With one measurement per item
  the four rules collapse to one: the item's own z.
- **Studentize is a DISPLAY toggle, default OFF** (checkbox): recompute (z − m)/σ over all
  items measured so far at publish time, never rewriting stored z. Flippable mid- and
  post-session — both views of the same data, which is what "I will test it" needs.
  `loop_sigma`-equivalent (per-block σ) and per-block mean stay published; without them the
  corrected p is uninterpretable after the fact.
- **Results view: Top-10 / Bottom-10 by z** (the existing `top[]`/`low[]`, displayed again)
  plus the Bonferroni line with `comparisons = items_done`. **Coverage and the most-frequent
  row are deleted** (`publish_coverage()`, `cover[]`/`cover_low[]`, `fm`/`fe` histograms).
  Per-block `drift_t` gets surfaced on the results screen, not only in `/loops`: with raw z,
  slow drift is the one thing that widens the extremes, and random order only stops it
  *attaching* to particular numbers.
- **Stated once, on the record**: at 7920 comparisons the null's expected max |z| ≈ 3.8 and
  Bonferroni p < 0.05 needs ≈ 4.5, so the significance line will read "not significant"
  essentially always — correctly. This is a selection instrument by design (user, 2026-08-02:
  "this program goes beyond science"); the corrected-p line stays as the honest label.

### 2.4 Export and API

- **`GET /results.csv`** on the master: streams every item measured so far — header comment
  (mode, focus, studentize-view, items_done/total, fw), then
  `idx,n1..n6,e1,e2,z_raw,block,order`. Live mid-session, still there after an abort.
  ⚠ RAM only: a master reboot loses it, so pull it periodically during a long session.
- **Removed params answer 400**, not silence: `?loops=`, `?rank=`, `?runs=` — the
  ignored-parameter-that-looks-like-a-working-one bug class is already on the record.
  `?mode=`, `?baseline=`, `?cal=`, `?calint=`, `?focus=`, `?confirm=` stay.
- The slot→combo stride mapping (`nth_combination` spreading a capped run) goes with the cap:
  uncapped, slot i **is** combination i.

## Workflow

Planning/architecture: Fable/Opus — this document is the contract. Implementation: Sonnet, one
task per session. Escalate back if a gate fails twice or a decision above is missing. Commit at
every green gate; the master and slave repos must be committed and flashed together whenever the
shared `components/` or the wire protocol changes.

**Start every session by reading `CLAUDE.md`'s "Where things stand"**, then §1.15–§1.17 here.
That is the whole handoff — `CLAUDE.md` loads automatically, so it is the one file that must
never go stale. It has drifted into being *wrong* once already (it described two calibration
policies as open for a day after they were fixed), which is worse than being incomplete: a fresh
session reasons from it. **Update it at the end of a session, not just this file.**

**Capture before you restart anything.** `g_status` is RAM: `/loops` and `/status` are lost on
reboot *and* on starting a new session, which resets `PairAcc` and `loop_hist`. §1.14 lost a whole
arm's pairwise matrix this way. Snapshot both into `docs/data/<date>_<name>/` first.
⚠ **The directory was emptied on 2026-07-29** — the lighting is being rebuilt, so no session
recorded before that change is comparable to one recorded after it, and keeping the old sets around
only invites a pooled comparison that means nothing. It starts empty; the rule above still stands
for everything measured on the new hardware. Snapshot the exposure ladders (`/calibrate` per node)
with the first session on it — the ladder is the record of what the new optics actually are.

**Settled — do not re-litigate:**
- §1.7's decisions, **except decision 3 (the XOR fold), which was WITHDRAWN** on measured
  evidence; §1.10 and the comment in `camera_calibrate()` say why, and the withdrawal is itself
  now settled.
- Entropy is **photons only**. The on-chip TRNG was deleted from both firmwares and must not be
  reintroduced in any form (see the CLAUDE.md noise-source section).
- Camera hardware: the OV5647 is **not** the bottleneck — two nodes measure at the sampling limit,
  and capture already runs at RAW8 800×800 (~13 % of the sensor) because the pipeline is
  PSRAM-bound at a 640 KB diff per frame pair. More megapixels would *lower* the bit rate.

**Deferred by the user — do not start unasked:** the attended-vs-unattended (focus) comparison.
**Dropped by decision — do not re-propose:** node-drop test, camera-fault/reboot path,
camera-stall abort, restoring the master to USB power.

---

### 1.16 Configurable measuring time + duty-cycle limit (2026-08-02…03)

**Decision:** one continuous attention window per Focus item (scoring, baseline, measurement);
length is operator-set, not a compile-time constant. Default **5 s**, intentional blank **~40 %**
of that (5 → 2 s, 7 → ~3 s). UI label: **Measuring Time (s)** · per Focus item. API:
`POST /start?run=<s>&gap=<s>`.

**Why not arbitrary length:** wall time is segment-count × instantaneous camera rate. Rate falls
when the measurement task starves the extraction task (duty-cycle cliff). Longer segment counts
are **non-linear** in wall ms — a linear “5× for 5 s” overshot to ~7.5 s and ~79 % duty. The
practical limit is therefore **not RAM**; it is “does `focus_win_ms` stay near the request with
zero stalls?”

**Live cal (4-node array, `cal=0`):**

| request | intentional gap | mean `focus_win_ms` | measured gap | stalls / faults | nodes |
|---|---|---|---|---|---|
| 5 s (66000 segs, ref) | 2 s | **~4680 ms** | ~3.7 s (blank + collect) | 0 | 4/4 |
| 7 s (`?run=7&gap=3`) | 3 s | **~6400 ms** | ~6.2 s | 0 | 4/4 |
| 78350 segs (failed linear 5 s) | 2 s | **~7535 ms** (collapsed rate) | ~2.0 s | 0 | 4/4 |

Segment mapping uses `RUN_SEGS_REF=66000` ↔ `RUN_MS_REF=4680` in `sensor.h`. Re-check
`focus_win_ms` after any camera/rate change. Top/Bottom tables show **numeric p** (erfc), not
n.s. buckets. Normal firmware delivery is **OTA only** (`build.ps1` docs no longer advertise COM
flash as the workflow).
