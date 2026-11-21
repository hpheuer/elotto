---
name: start-params
description: Every /start key, its range and default, the 400 refusals, the measuring-time and gap rules, ?maxruns=0 (scoring-only) and ?passes=0 (continuous). Read before touching the /start parser or choosing session parameters.
---

# /start — keys, ranges, refusals

`/start` is a **whitelist**: an unknown key answers **400**. Deleted keys (`loops`, `runs`, `rank`, `focus`, `went`, `wruns`,
`baseline`, `calint`, `unlimited=0`, `wpre`) keep a specific 400. Allowed: `mode`, `run`, `srun`, `gap`, `gappct`,
`kdn`, `kmau`, `kser`, `kdisp`, `kbyt`, `ksync`, `kz`, `fix`, `maxruns`, `passes`, `confirm`, `cal`, `unlimited`, `fakepool` (TEST: `1` skips the scoring and draws a random pool). An empty key (`?=x`), a value over
`START_VAL_MAX` 31 characters and a query string over 255 characters each answer **400** as well
— a truncated value or query must never read as "key absent" and resolve to the default.
- ⚠ **A refused /start changes nothing**: parameters are parsed into locals and committed to
  `g_status` only after every one validates, so a 400 leaves the finished session's `/status` —
  the Z\* weights above all — describing that session. A 500 (task create failed) restores
  the snapshot, including `state`; `prefs_save()` runs only after the task exists. 100 % of the progress bar is the
  full combination space. `NUM_RUNS` 1400 is the hard cap on `results[]`.
- ⚠ **A combination space larger than `NUM_RUNS` aborts** rather than compacting mid-round.
  Eurojackpot's 7920 and 6-of-49's 5005 are both over it; the pool is sized down to `?maxruns=`.
  Compaction keeps at most 1000 rows (the ranked extremes) and the next round is appended after
  them, so `?maxruns=` is capped at `UNLIM_RUNS_MAX` 400 = `NUM_RUNS` − 1000 and a round is never
  truncated.

## Measuring time — one per phase
`?run=<s>` is the pass (per item), `?srun=<s>` the number scoring (per number; omitted = `?run=`); each **0,2–22 s**, `run` default
5; out of range answers **400**, no fallback. Each phase triggers its own segment count
(`run_segs` / `srun_segs` in `/status`). The 22 s ceiling is `RUN_S_MAX`; the wire allows `EL_SEG_MAX` 3000000 divided by the measured
rate (`EL_SEG_MAX / (RUN_SEGS_REF/RUN_MS_REF)` ≈ 22,5 s), not a camera limit; the rate is measured to 14,7 s.
`?gap=<s>` if present must be 0–10 s, else **400**, and applies to both phases. `?gappct=<p>` (whole
percent 0–100, else **400**) sets each phase's gap to p % of its own window, capped at 10 s; both
keys at once answer **400**. Neither → `GAP_AUTO_PCT` 40 % of each phase's own window
(`gap_s` / `sgap_s`), no floor. A gap of 0 is legal — the flush on `M`, not the gap, separates items.
The start form sends `?gappct=` (field **Gap (%)**, starts at `GAP_PCT_FORM_DEFAULT` 5).
The sweep's mini-runs use the pass window; in a `?maxruns=0` session the scoring window.
- ⚠ z is normalised by √segments, so a number's scoring z and an item's z are comparable in unit;
  the scoring sums and the pass blocks are never mixed anyway.
- `?cal=` 0..`CAL_BUDGET_MAX_MS` (0 = no sweep), `?maxruns=` **0 or 10..400** (`UNLIM_RUNS_MAX`); out of range **400**, no fallback.
- **`?maxruns=0` is scoring-only**: no combination pass, the session just scores numbers round after round,
  until Abort or the 72 h limit — `results[]`, blocks, compaction, drift and the pairwise matrix are
  bypassed entirely (no pass → none of their counters move), no pool is picked (`pool_n_main/euro`
  stay 0), and the Top-10 comes from the sorted scoring table.
- ⚠ **`?passes=0` is the continuous-scoring flag and is legal ONLY with `?maxruns=0`** (else **400**): one never-ending
  scoring, one pass per round, sums **accumulate without reset** (one snap per pool, refreshed),
  whereas `?passes≥1` with `?maxruns=0` re-scores from scratch each round.
- Segment count follows from `RUN_SEGS_REF`/`RUN_MS_REF` in `elotto_link.h` (the nodes' time slots and the master's reply wait derive from the same pair; after a change flash every node), so nominal `?run=` matches the real
  `focus_win_ms` (short windows run a touch long by the fixed per-window onset overhead). ⚠ The
  per-node rate still varies, so `focus_win_ms` is set by the **slowest** node.
