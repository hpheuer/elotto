# PLAN: elotto — the live contract

**Start here:** `CLAUDE.md`'s "Where things stand" section is the one-screen summary of the rig and
the open threads in priority order. This document is the detail behind it.

⚠ **§1.1–§1.20 were removed on 2026-08-17.** They documented the v2 loop/ranking era and the
pre-2026-07-29 optics — an instrument that no longer exists — and every rule from them that still
applies now lives in `CLAUDE.md`. They are in git history at **`998c7ab`**
(`git show 998c7ab:docs/PLAN.md` — `144ed5e` before the 2026-08-20 history rewrite), and the closed design findings §1.1–§1.14 are also still in
[`PLAN_HISTORY.md`](PLAN_HISTORY.md). Citations of the form "§1.x" in source comments resolve to
those, not to a file on disk.

⚠ `PLAN_4NODE.md` and `PLAN_NETWORK.md` were deleted earlier at the user's request and are in git
history, last present at **`8e134e5`** (`git show 8e134e5:docs/PLAN_4NODE.md`). Source comments
still cite them by name; those citations are historical too, and were left alone deliberately.

---

## 2 v3 — the single-pass session (specified and implemented 2026-08-02)

**The core rule: no Focus item is ever measured twice.** Phase 0 obeys it (one long window per
number) and Phase 2 obeys it too, and everything below follows from that one decision. A v3 session
must never be pooled with any v2.x session.

### 2.1 Session shape

Calibrate + baseline → observer gate (`/ready`, attended only) → Phase 0 scoring → pool
confirmation → **ONE pass over every combination in the confirmed pool, each measured exactly
once**, in a fresh Fisher–Yates random order. Then done. **No loops, no loop counter, no `Runs`
cap.** The progress bar's 100 % is the full combination count; beside it an **items counter**
(`items_done / full_combos`) replaces the loop badge.

- **Window/gap: the same one every phase uses**, operator-set — `?run=` (default 5 s) and `?gap=`
  (default 40 % of run). The segment count travels on the wire, so this is a master-only setting;
  no slave reflash. Duty cycle, not window length, is what starves the extraction task.
- **Session length**: measured live at `run=5`/`gap=2`, a 6-of-49 pass of 5005 items takes
  **~12,2 h** of measuring (window 4,48–4,55 s, gap 4,29 s, cycle ~8,8 s) and ~13,4 h wall with
  insertions. Eurojackpot's 7920 items scale from the same cycle. **Attended by assumption**: the
  observer peeks, lets the rest run subconscious, and uses **Pause** (clock stops, excluded from
  `elapsed_ms`) or **Abort** (partial results published from the measured prefix) at their own
  judgment. No time budget.
- **Cap**: `NUM_RUNS` stays **8000**. Both pools already fit (7920 / 5005).

### 2.2 Blocks replace loops as the statistics unit

**Every ~15 min** (default; `?calint=`) the pass parks and runs **sweep + baseline together**, then
resumes. That boundary closes a **block**, which inherits everything that was per-loop:
`record_loop()` → per-block row in `/loops` (endpoint name kept), `drift_add()` on the block's
master mean, `pairs_fold_loop()` for the per-block-centered pairwise matrix — **and since
2026-08-13 the block centring of §3.1, which is now the block's most important job.**

At ~100–205 items per block a pass gets comfortably past `DRIFT_MIN_LOOPS` = 6 at ~6 % overhead.
The cadence is therefore three things at once: drift resolution, insertion overhead, **and the
sample size each centring mean is estimated from.** Shortening it sharpens drift tracking but makes
the centring noisier; lengthening it does the reverse.

- Baseline runs at the **measurement length** — same-instrument rule — and is **drift reference
  ONLY**: the `zm = zraw − baseline_mean` subtraction is removed. With raw z published it would
  have been a live, master-only asymmetry.

### 2.3 z, ranking, results

- **Stored z is always RAW** (the combined Σz/√k, nothing subtracted, nothing rescaled).
  `RANK_*`, the combobox, `s_zsum`/`s_zmin`, `accum_reset()`, `publish_cumulative()`,
  `publish_extreme()`, `absorb_loop()` — all deleted. With one measurement per item the four
  ranking rules collapse to one: the item's own z.
  ⚠ **What the ranking RUNS on is no longer this value** — see §3.1. `z_score` remains the archive;
  `z_ctr` is the statistic.
- ⚠ **The studentize checkbox specified here was NEVER BUILT.** What exists instead: `/status`
  publishes `pass_mean` and `pass_sigma`, and the nearest-zero table shows a **`Z*` column**. The
  CSV summary carries the same `z_std`. Both views remain derivable from either file forever; only
  the global flip-a-checkbox UI is absent. Do not cite the checkbox as a feature.
- **Results view: Top-5, Bottom-5 and Nearest-zero-5** (`top[]`/`low[]`/`near[]`) plus the
  Bonferroni line. **Coverage and the most-frequent row are deleted.**
  ⚠ **Nearest-zero is nearest the PASS MEAN, not nearest raw 0**: z carries the array's common
  offset, so |z| ≈ 0 would select items well off the array's own centre. `results_near_mean()` picks
  by |z − mean|, which orders identically to |z − mean|/σ, and recomputes on demand rather than
  accumulating — the mean moves as the pass proceeds, so an item admitted against an early mean
  would be judged against a number that no longer exists.
  Per-block `drift_t` is surfaced on the results screen, not only in `/loops`.
- **Stated once, on the record**: at 7920 comparisons the null's expected max |z| ≈ 3,8 and
  Bonferroni p < 0,05 needs ≈ 4,5, so the significance line will read "not significant" essentially
  always — correctly. This is a selection instrument by design; the corrected-p line stays as the
  honest label.

### 2.4 Export and API

- **`GET /results.csv?all=1`**: streams every item measured so far — header comment (mode, focus,
  score direction, ranked/excluded/void counts, blocks, `pass_mean`, `pass_sigma`, `pass_chi2`,
  `pass_stouffer`, `v_eff`, `null_flags`, and the node IP list in column order), then
  `order;item;n1..n6;e1;e2;z_raw;z_ctr;block;k;skip_rank;z0..z3`. Live mid-session, still there
  after an abort. ⚠ RAM only: a master reboot loses it, so pull it periodically.
- **Bare `GET /results.csv` is the 15-row summary**: the three published groups — `high` / `low` /
  `zero`, five each — as `group;rank;item;n1..n6;e1;e2;z_raw;z_std;z_ctr;block;k`. The Save button
  fetches this; `?all=1` sits below it as a plain link, deliberately. ⚠ **The summary is not the
  record.** Fifteen rows cannot be re-derived into a pass and no item is ever re-measured, so the
  archival pull stays the operator's explicit act rather than a side effect of clicking Save.
- **The session's parameters stay on screen while it runs.** The form is hidden once a session
  starts and a curl-started one never had a form, so the progress area carries a read-only
  parameter line: mode, `run_s`, `gap_s`, `run_segs`, seconds per run, measured `focus_win_ms` /
  `focus_gap_ms`, `baseline_total`, `cal_budget_ms` + `cal_interval_ms`, `score_dir`, focus, and
  the unlimited cap. Sourced from `/status` so the device's numbers are what is shown, and kept
  after `done`/`aborted` so a screenshot can be matched to its CSV. Seconds per run is MEASURED
  (`elapsed_ms / (baseline_done + scoring_done + completed)`) once ≥ 5 runs exist and the
  `CYCLE_RUN_PCT` estimate before that, labelled either way. Live check 2026-08-18 against a
  running 6-of-49 session: `per run 8,6 s measured`, `window/gap 4491 / 4288 ms` — against the
  model's 8,8 s, which is the second confirmation of `CYCLE_RUN_PCT`.
- **German CSV throughout**: `;` separator AND `,` decimal. Both halves are the decision — with a
  decimal point in the cell, German Excel reads the whole column as text.
- **Removed params answer 400**, not silence: `?loops=`, `?rank=`, `?runs=`. `?mode=`, `?baseline=`,
  `?cal=`, `?calint=`, `?focus=`, `?run=`, `?gap=`, `?score=`, `?confirm=` stay.
  ⚠ **`?mode=` is `1` for 6-of-49 and ANYTHING ELSE for Eurojackpot** — it tests `val[0]=='1'`, so
  `?mode=649` silently starts a Eurojackpot session. Documented rather than fixed, because the UI
  sends the right value and changing the encoding would break a curl script written against it.

---

## 3 Block-centred ranking and the health machinery (2026-08-13 … 08-17)

Driven by the **2026-08-13 unattended full pass** (`docs/data/2026-08-13_6of49_fullpass_unattended/`,
5005 items). Its finding, in one line: **per-node σ ≈ 1,0 inside every block while the block offsets
jumped by several z.** The noise was fine; the zero point moved.

### 3.1 The ranking runs on a block-centred combine

At every block close `center_block()` subtracts each node's own mean over that block and recombines
over the same nodes (`have_mask`, so k and the √k scaling are unchanged), writing `z_ctr`. Pass
mean/σ/χ², Top-N, Bottom-N, nearest-zero and Bonferroni all read it through one accessor,
`rank_z()`. Until a block closes, `z_ctr` carries the provisional raw value.

`z_score` stays **raw and untouched** — the uncentred view survives in the CSV forever. `z_adj`
(z/√v_eff) is **gone**: it divided by a `v_eff` measured in the *previous* block, so the column
mixed scales down its own length.

**Evidence**: re-centred, the 08-13 pass is a clean null — max |z| **3,92** against the **4,13**
expected as the maximum of 5005 normal draws, at σ 0,995. Uncentred it published a Top-5 in which
all five items came from one block and four of five Bottom items from another.

⚠ **Centring removes any real effect that is CONSTANT across a whole block.** Pre-registration
decision, not a detail: what this instrument can still see is an effect varying **between items
inside a block**. It also shrinks σ by (1 − 1/n_block) — ~0,5 % at ~100 items — so σ(z_ctr) sits
slightly under 1 by construction.

### 3.2 Soft-down may now take more than one node

`NODE_SOFT_MIN_COMBINE` went **3 → 1**. At four nodes a floor of 3 allowed exactly ONE exclusion, so
when two arms misbehaved the second stayed in. In the 08-13 pass slave1 was excluded and the master
— 9 of 49 blocks with |mean| > 1,5, worst −6,33 — was kept, and block 4 published
(−6,33 + 0,27 + 0,22)/√3 = **−3,38**, exactly the value in the results.

A bad arm costs more than a small k (user decision, 2026-08-13). Up to three of four may now drop
out, so a solo combine is possible; `k` is in the CSV per item so it is visible afterwards.

### 3.3 Quarantine fires on every contaminating trip

A block whose data a tripping node could have contaminated is excluded from ranking
(`skip_rank=1`) while staying in the CSV. It fires on **every** trip, but **only when that node was
actually in the block's combine** — `center_block()` sets the contribution mask on the pass it
already makes. Both simpler rules were tried and both are wrong:

| rule | outcome |
|---|---|
| only the node's FIRST trip | later bad blocks waved through — 08-13 sealed blocks 1, 24, 33 but ranked 4, 14, 16 |
| every trip, unconditionally | 33 of 33 items excluded per block, three blocks running, pass σ 0,000 — nothing left to rank |

### 3.4 Unattended sessions do not stop for a click

The observer gate is armed on **`focus_mode`**, not merely on `confirm=1`. It has no timeout by
design, and the web UI sends `confirm=1` unconditionally — which parked the 08-13 unattended pass
behind a Start button nobody was there to press: **37,9 h wall against 12,2 h of measuring, i.e.
~25,7 h stalled at a gate** (`pool_confirm=1`, `pool_auto=0`, `focus=false` in its `status.json`).
No observer, no observer gate.

The pool gate takes the proposal **immediately** when `focus=0` and records `pool_auto=1` — the same
flag the 15-minute timeout sets, so the CSV can never claim a human approved the pool.

### 3.5 Verified on hardware (2026-08-17)

Short session, `confirm=1&focus=0`, `run=2`/`gap=1`/`calint=2 min`: gates skipped, `pool_auto=1`,
closed blocks at mean(z_ctr) **exactly 0,0000** while z_raw sat at −0,55 / +0,08 / +0,04, open block
carrying the provisional raw value, 0 quarantined, 0 void, pass σ 1,025, `null_flags` 0, all four
nodes in the combine. An earlier run of the same configuration put two nodes soft-down
simultaneously, which the old floor of 3 could not have done.

**Not yet exercised:** centring across a full-length pass with 15-minute blocks.

---

## 4 Unlimited Mode — rounds instead of one pass (specified and implemented 2026-08-18)

Requested by the user on 2026-08-18. **A session that does not end with a combination space.**
Opt-in: UI checkbox **"∞ Unlimited mode (rounds until Abort)"** with a **Runs per round** field
(default **100**), on the wire `POST /start?unlimited=1&maxruns=<n>`.

### 4.1 The round

score every number → **keep only as many of the best-scoring as fit `maxruns` measurement runs** →
measure that whole (small) space once, fresh Fisher–Yates → score again → repeat.

It stops on **Abort**, or when `results[]` reaches `NUM_RUNS` = 8000, which finishes the session
DONE with the reason in `fault`. There is no other terminating condition, by design.

Compared with §2.1 the shape is otherwise unchanged: the same window/gap for every phase, the same
`?run=`/`?gap=`, the same observer gate (once, at session start, attended only). The pool
confirmation gate is **skipped** — choosing the pool by score is what the mode is — and recorded as
`pool_auto=1`, exactly like every other selection no human approved.

Rounds after the first re-run **sweep + baseline before scoring** (skipped at `?calint=0`): the
scoring runs are what choose the pool, so they must not sit on a sweep from an hour ago.

### 4.2 Pool sizing: maximise the combinations measured

`unlimited_pool_sizes()` in `sensor.c`. **The objective is the combination count, and only that.**

⚠ **A weighted rule shipped first and was withdrawn the same day** (2026-08-18) after the user
asked whether it could hurt the draw probability. It can, and the arithmetic settles it:

```
P(pool contains the real draw) = C(p,5)/C(50,5) · C(q,2)/C(12,2)
                               = [C(p,5)·C(q,2)] / 139 838 160
                               = combinations measured / combinations that exist
```

The main/bonus split **cancels out completely** — only the product survives, and the product is
the run count. So any pool rule is neutral exactly when it spends the budget, and harmful exactly
in proportion to the runs it leaves unspent. It costs twice over: a short round also reaches its
next 62-run scoring pass sooner, so the scoring overhead per measured item rises too.

The withdrawn rule maximised universe coverage `p/50 + q/12` = `12p + 50q`, a bonus number weighted
~4× because there are only 12 against 50. It pinned q at 5 for every cap ≥ 10 and lost badly:

| cap | withdrawn rule | current rule | coverage lost |
|---|---|---|---|
| 50 | 5+5 → 10 | 6+4 → 36 | **3,6×** |
| 100 | 6+5 → 60 | 7+3 → 63 | 1,05× |
| 250 | 7+5 → 210 | 7+5 → 210 | — |
| 500 | 7+5 → 210 | 11+2 → 462 | **2,2×** |
| 1000 | 8+5 → 560 | 12+2 → 792 | 1,41× |
| 2000 | 9+5 → 1260 | 10+4 → 1512 | 1,20× |
| 7920 | 12+5 → 7920 | 12+5 → 7920 | — |

**The bonus-number preference survives as the TIE-BREAK**, and only there: equal combination count
→ larger bonus pool, then larger main pool. At an equal count P is identical, so the preference is
free — which is precisely why it is a tie-break and not the objective.

**6-of-49 was never affected**: with no second pool, more combinations *is* more numbers, so the
same objective yields the same answer it always did (cap 100 → 9 numbers, 84 combinations).

Current values:

| cap | Eurojackpot | 6-of-49 |
|---|---|---|
| 50 | 6+4 → 36 runs | 8 → 28 runs |
| 100 | 7+3 → 63 runs | 9 → 84 runs |
| 500 | 11+2 → 462 runs | 11 → 462 runs |
| 2000 | 10+4 → 1512 runs | 13 → 1716 runs |
| 7920 | 12+5 → 7920 runs | 15 → 5005 runs |

At a large enough cap the mode degenerates into **repeated full passes**, which is the correct
limiting behaviour.

**Stated once, for scale**: even the full 7920-combination pool covers 1 draw in ~17 700. No pool
rule changes that order of magnitude; what is at stake here is a factor of 2–3 inside a very small
number, and the reason to take it is that it is free.

### 4.3 Results accumulate; the "once" rule is relaxed across rounds

`results[]` is **never cleared between rounds**. Top-5 / Bottom-5 / Nearest-zero, pass mean/σ/χ²
and the Bonferroni line all run on the union of every round so far, so a later round's items sort
straight into the same tables — which is the point of the feature.

⚠ **This is the one place v3's core rule is relaxed, and it is a pre-registration decision.** Inside
a round every combination is still measured exactly once. **Across** rounds a combination can
recur, because a later scoring pass may pick overlapping numbers. Each recurrence gets its own row;
nothing is averaged, merged or overwritten. Consequences that must not be forgotten later:

- `index` (combination id) is meaningful **only within its round** — the pool it enumerates changes
  every round. The identity is **(round, index)**; `n1..n6;e1;e2` is unambiguous either way.
- `comparisons` for the Bonferroni line counts rows, so a repeated combination counts twice. It is
  two measurements, but they are not two independent hypotheses.
- **Never pool unlimited-mode data with a single-pass session** without splitting on `round`, and
  never pool across rounds without deciding what repeated combinations mean for the analysis.

**Every round closes its own block**, even at `?calint=0` — a round boundary has a full re-scoring
sitting in it, so centring must never span one.

### 4.4 API and export

- `/status`: `unlimited`, `runs_cap`, `round`, `round_base`, `round_done`, `round_total`.
  `completed` stays **session-wide** (the `results[]` prefix); `total` is the **current round**.
  ⚠ A progress bar must use `round_done/round_total`, or its denominator moves under a growing
  numerator. ETA is to the end of the **round**, which is the only end there is.
- `/status` now publishes the pool being measured **for the whole of every running session**
  (`pool_main`/`pool_euro`), not only while the confirmation gate asks about it — a session started
  without `?confirm=` used to measure a pool `/status` never named.
  ⚠ **`sensor.c` withdraws the pool at the ROUND BOUNDARY (`round++`), not when the scoring pass
  begins, and at session start BEFORE `state` goes RUNNING.** Both were wrong on the first build
  and both showed the same symptom — correct numbers under the wrong round. The sweep + baseline
  insertion sits between `round++` and scoring and takes a minute or more with `round` already
  advanced; and at session start the opening sweep, baseline and observer gate all run before any
  scoring, so the PREVIOUS SESSION's pool was on screen for all of them.
  The UI renders it as an info line **directly under the Number-scoring bar** — the same number
  chips as the result tables and the Focus panel, so a bonus number reads as a star there too —
  labelled `Round N numbers (9):` in unlimited mode and `Selected numbers (12+5):` otherwise.
- The **Runs per round** field carries a live estimate to its right: the pool each budget buys AND
  **how long one round takes** at the parameters currently in the form, both modes, two lines —
  `Euro 7+3 = 63 runs · ≈ 19 min/round` / `6of49 9 = 84 runs · ≈ 21 min/round`. The operator sets a
  run BUDGET, not a duration, and 100 runs is ~20 min at `run=5` but over an hour at `run=15`; in
  this mode there is no session end to discover that from afterwards. It re-computes as the run
  window and the baseline count are typed, not only the budget.
  The model is `CYCLE_RUN_PCT` in `sensor.h` — `cycle_ms ≈ 1,36 · run_ms + gap_ms`, a FIT to two
  live 4-node measurements (`run=5`/`gap=2` → 8,8 s; `run=1`/`gap=0,5` → ~1,86 s), because the
  window comes out ~10 % short and the gap carries the slave collect (~46 % of the window) on top
  of the requested blank. A round = scoring sweep (49 or 62 runs) + the pool's combinations + one
  sweep+baseline insertion at the boundary + one more per `calint` of MEASURING time (which is what
  the device's block timer counts). Checked against the `maxruns=20` smoke test: model 2 min,
  measured ~2 min. ⚠ An estimate — long windows stretch further as the camera rate falls under
  duty cycle, which is what `?run=` exists to probe. Once a session is live the ETA comes from the
  device's measured pace instead.
- CSV header gains `unlimited=on|off runs_cap=<n> rounds=<n>`, and `items=` reads
  `<measured>/<end of the current round>` so it stays monotone.
- `?all=1` gains a **`round` column, APPENDED last** — the existing columns keep their positions so
  an analysis script written against an older file still parses.
- `?runs=` still answers **400**; the message now names `unlimited=1&maxruns=` as the replacement.

### 4.5 Verified on hardware (2026-08-18)

`mode=649&run=1&gap=0.5&baseline=10&cal=0&calint=0&unlimited=1&maxruns=20&focus=0`, four nodes.
Cap 20 → **7 numbers, C(7,6) = 7 combinations** per round, as the rule predicts. Observed:

- round 1 pool `18 21 28 40 45 46 48`, round 2 pool `20 26 27 29 31 32 39` — **re-scored, different**;
- `completed` 7 → 14 across the boundary, `round_done` resetting to 0/7 each round, `blocks` 1 → 2;
- the pool blanked in `/status` during each scoring phase and republished at the round start;
- `?all=1` after round 2: 14 rows, 7 per round, `block` 0/1 matching `round` 1/2, every row `k=4`,
  header `unlimited=on runs_cap=20 rounds=3`, `round` column last;
- both closed blocks centred → `pass_mean` **−0,000000**, `null_flags` 0, 0 void, 0 quarantined;
- Abort during round 3's scoring kept all 14 measured items.

Third run, after the pool rule was corrected — **Eurojackpot** at `maxruns=50`, the sharpest
discriminator there is (the withdrawn weighted rule answers 5+5 = 10 combinations, the current one
6+4 = 36). The device sized the round at **`round_total` 36, `pool_main` 6 numbers, `pool_euro` 4**,
i.e. the corrected rule, and the served page's `unlimPool()` carries the same objective and
tie-break with no weights left in it.

Second run after the pool info line was added: the line was **blank through every scoring pass**,
showed `17 32 33 34 36 40 48` while round 1 measured, blanked again at the round boundary, and came
back as `6 8 17 25 26 35 44` for round 2 — a different pool, as it must be. `pool_main` is absent
from `/status` once the session ends, so the line hides itself and the confirmation modal still
cannot be raised over a dead session.

**Not yet exercised:** a long unlimited run with real 15-minute blocks and `cal=1`, and the
`results[]`-full stop at 8000 items.

---

## Workflow

Planning/architecture: Fable/Opus — this document is the contract. Implementation: Sonnet, one task
per session. Escalate back if a gate fails twice or a decision above is missing. Commit at every
green gate; the master and slave repos must be committed and flashed together whenever the shared
`components/` or the wire protocol changes.

**Start every session by reading `CLAUDE.md`'s "Where things stand"**, then §3 here. That is the
whole handoff — `CLAUDE.md` loads automatically, so it is the one file that must never go stale. It
has drifted into being *wrong* once already (it described two calibration policies as open for a day
after they were fixed), which is worse than being incomplete: a fresh session reasons from it.
**Update it at the end of a session, not just this file.**

**Capture before you restart anything.** `g_status` is RAM: `/loops` and `/status` are lost on reboot
*and* on starting a new session, which resets `PairAcc` and `loop_hist`. A whole arm's pairwise
matrix was lost this way once. Snapshot `/status`, `/loops`, `/results.csv?all=1` and the four
`/calibrate` ladders into `docs/data/<date>_<name>/` **before** starting anything else.
⚠ This has failed twice in practice, both times because the pull happened but the files stayed in a
temp directory: three unrepeatable datasets were found outside version control on 2026-08-17.
Archive into the repo, with a README naming the firmware SHA, in the same sitting.

**Settled — do not re-litigate:**
- Entropy is **photons only**. The on-chip TRNG was deleted from both firmwares and must not be
  reintroduced in any form.
- Camera hardware: the OV5647 is **not** the bottleneck — capture already runs at RAW8 800×800
  (~13 % of the sensor) because the pipeline is PSRAM-bound. More megapixels would lower the bit
  rate.
- Power topology: all four nodes on PoE, permanently. The master's separate USB supply will not be
  restored.
- Per-block calibration is **statistically neutral** (A−B = 0,52 SE over 6×430 runs per arm). The
  open question about it is the *rate of bad blocks*, not the variance it adds.
