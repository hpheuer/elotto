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


---

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
arm's pairwise matrix this way. Snapshot both into `docs/data/<date>_<name>/` first — several runs
are already archived there, including 89 loops from an aborted 400-loop session that **nobody has
analysed yet**.

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
