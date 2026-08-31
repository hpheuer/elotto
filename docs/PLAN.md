# PLAN: elotto — the live contract

**Start here:** [`STATUS.md`](STATUS.md) for the one-screen snapshot; [`../CLAUDE.md`](../CLAUDE.md)
for rules. This document is the **v3 contract detail** behind them.

⚠ **§1.1–§1.20 (v2 era) are gone from this file.** Closed findings §1.1–§1.14: stub
[`PLAN_HISTORY.md`](PLAN_HISTORY.md) → `git show 1e62bca:docs/PLAN_HISTORY.md`. Source comments
`§1.x` resolve there. `PLAN_4NODE.md` / `PLAN_NETWORK.md`: `git show 8e134e5:…`.

---

## 2 v3 — the single-pass session (specified and implemented 2026-08-02)

**The core rule: no Focus item is ever measured twice.** Phase 0 obeys it (one long window per
number) and Phase 2 obeys it too. A v3 session must never be pooled with any v2.x session.

### 2.1 Session shape

Sweep → Phase 0 scoring → pool confirmation (`confirm=1`) → **ONE
pass over every combination in the confirmed pool, each measured exactly once**, Fisher–Yates
order. Then done. **No loops, no `Runs` cap.**

- **Window/gap:** `?run=` **0,5–5 s** (default 5) `[D51]`, `?gap=` default 40 % of run (floor 0,5 s).
  Segment count on the wire; master-only setting.
- **Session length (order of magnitude):** at `run=5`/`gap=2`, 6-of-49 ≈ 12 h measuring. Pause /
  Abort as in CLAUDE. Cap `NUM_RUNS` **8000** (Euro 7920 needs one compaction near the end `[D45]`).

### 2.2 Blocks replace loops as the statistics unit

**Every ~15 min** (`?calint=`) the pass parks for the **camera sweep**, then resumes. That boundary
closes a **block**: `/loops` row, drift point, pairwise close, **block centring** (§3.1) — the
block's most important job since 2026-08-13.

Cadence ≈ drift resolution + insertion overhead + sample size for each centring mean.

### 2.3 z, ranking, results

- **Stored z is always RAW** (the combined Σz/√k). `z_score` is the archive; **`z_ctr` is the
  statistic** after block centring (§3.1). Ranking key is z plus concordance `[D65]`, each
  channel divided by **that item's block σ** `[D68]`. Z* **is** the key. Scoring centres
  per node over its own span, then the same mix `[D69]`.
- **Results:** Top-5, Bottom-5 (`Z*`, `Z`, `Conc`), jump board, soft-down origins. `drift_t` on
  the results screen. `/loops` carries `rank_sig_p` / `rank_sig_c` per block.

### 2.4 Export and API

- **`GET /results.csv?all=1`**: streams every item measured so far — header as in CLAUDE, then
  measurement-order rows `z_raw`/`z_ctr`/`key`/`zc_ctr`/`w0..w3`. Live mid-session. ⚠ RAM only —
  pull periodically. Bare `/results.csv` = 15-row summary, **not** the archive (Save → `?all=1`).
- **Parameter line** from `/status` while running (and after done/abort): mode, `run_s`/`gap_s`/
  `run_segs`, measured pace, cal, score, unlimited, concordance weight.
- **German CSV**: `;` separator, `,` decimal.
- Unknown start parameters answer **400**. ⚠ `?mode=` is `1` = 6-of-49, anything
  else = Eurojackpot (`val[0]=='1'`).

---

## 3 Block-centred ranking and the health machinery (2026-08-13 … 08-17)

Driven by the **2026-08-13 unattended full pass** (5005 items). Its finding, in one line:
**per-node σ ≈ 1,0 inside every block while the block offsets
jumped by several z.** The noise was fine; the zero point moved.

### 3.1 The ranking runs on a block-centred combine

At every block close `center_block()` subtracts each node's own mean over that block and recombines
over the same nodes (`have_mask`, so k and the √k scaling are unchanged), writing `z_ctr`. Pass
mean/σ/χ², Top-N and Bottom-N all read it through one accessor,
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

### 3.4 Always unattended `[D66]`

No observer gate. The HTML card shows the current number (scoring) or combination (pass).
`?focus=` answers 400. CSV `focus=off`.

No pool gate `[D67]`. The score sizes the pool to `maxruns`; `pool_auto=1`.

### 3.5 Hardware check

Centring / soft-down / quarantine verified on short unattended runs 2026-08-17; narrative trimmed
2026-08-28 → `git show 1e62bca:docs/PLAN.md`. Live open points: [`STATUS.md`](STATUS.md).

---

## 4 Unlimited Mode — rounds instead of one pass (specified and implemented 2026-08-18)

Requested by the user on 2026-08-18. **A session that does not end with a combination space.**
**D67:** this is the only session. **Runs per round** field
(default **100**), on the wire `POST /start?unlimited=1&maxruns=<n>`.

### 4.1 The round

score every number → **keep only as many of the best-scoring as fit `maxruns` measurement runs** →
measure that whole (small) space once, fresh Fisher–Yates → score again → repeat.

It stops on **Abort**, or when `results[]` reaches `NUM_RUNS` = 8000, which finishes the session
DONE with the reason in `fault`. There is no other terminating condition, by design.

Compared with §2.1 the shape is otherwise unchanged: the same window/gap for every phase, the same
`?run=`/`?gap=`. The pool confirmation gate is **skipped** — choosing the pool by score is what
the mode is — and recorded as `pool_auto=1`, exactly like every other selection no human approved.

Rounds after the first re-run the **sweep before scoring** (skipped at `?calint=0`): scoring
chooses the pool and must not sit on a sweep from an hour ago. (Baseline deleted `[D48]`.)

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
all run on the union of every round so far, so a later round's items sort
straight into the same tables — which is the point of the feature.

⚠ **This is the one place v3's core rule is relaxed, and it is a pre-registration decision.** Inside
a round every combination is still measured exactly once. **Across** rounds a combination can
recur, because a later scoring pass may pick overlapping numbers. Each recurrence gets its own row;
nothing is averaged, merged or overwritten. Consequences that must not be forgotten later:

- `index` (combination id) is meaningful **only within its round** — the pool it enumerates changes
  every round. The identity is **(round, index)**; `n1..n6;e1;e2` is unambiguous either way.
- A repeated combination across rounds is two measurements, not two independent hypotheses.
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
  and both showed the same symptom — correct numbers under the wrong round. The sweep insertion
  sits between `round++` and scoring; at session start the opening sweep runs
  before scoring, so a stale pool must not linger on screen through it.
  UI: info line under the scoring bar (`Round N numbers` / `Selected numbers`).
- **Runs per round** field: live pool size + round-length estimate from the rate model in
  `sensor.h` (`cycle_ms ≈ 224·segments/rate + CYCLE_FIXED_MS + gap_ms` `[D39]`). Live ETA uses
  measured pace once enough runs exist.
- CSV header gains `unlimited=on|off runs_cap=<n> rounds=<n>`, and `items=` reads
  `<measured>/<end of the current round>` so it stays monotone.
- `?all=1` gains a **`round` column, APPENDED last** — the existing columns keep their positions so
  an analysis script written against an older file still parses.
- `?runs=` still answers **400**; the message now names `unlimited=1&maxruns=` as the replacement.

### 4.5 Hardware check

Unlimited pool sizing / round boundaries / Abort-prefix verified 2026-08-18 (`maxruns=20` 6-of-49,
Euro `maxruns=50` → 6+4). Narrative trimmed 2026-08-28 → `git show 1e62bca:docs/PLAN.md`. Compaction
at full buffer: `[D42]`.

---

## Workflow

**Start every session with [`../CLAUDE.md`](../CLAUDE.md) + [`STATUS.md`](STATUS.md).** This file is
the contract detail; update CLAUDE when a rule changes, DECISIONS when evidence moves.
Master and slave repos commit/flash together when `components/` or the wire changes.

**Capture before you restart anything.** `g_status` is RAM: `/loops` and `/status` are lost on reboot
*and* on starting a new session, which resets `PairAcc` and `loop_hist`. A whole arm's pairwise
matrix was lost this way once. Snapshot `/status`, `/loops`, `/results.csv?all=1` and the four
`/calibrate` ladders into `docs/data/<date>_<name>/` **before** starting anything else.
⚠ This has failed twice in practice, both times because the pull happened but the files stayed in a
temp directory: three unrepeatable datasets were found outside version control on 2026-08-17.
Archive into the repo, with a README naming the firmware SHA, in the same sitting.

**Settled — do not re-litigate:**
- Entropy is **photons only**. There is no second source and must not be
  reintroduced in any form.
- Camera hardware: the OV5647 is **not** the bottleneck — capture already runs at RAW8 800×800
  (~13 % of the sensor) because the pipeline is PSRAM-bound. More megapixels would lower the bit
  rate.
- Power topology: all four nodes on PoE, permanently. The master's separate USB supply will not be
  restored.
- Per-block calibration is **statistically neutral** (A−B = 0,52 SE over 6×430 runs per arm). The
  open question about it is the *rate of bad blocks*, not the variance it adds.
