# elotto – ESP32-P4 Project

**Rules live here. Evidence lives in [docs/DECISIONS.md](docs/DECISIONS.md)**, cited as `[D<n>]`.
Change a rule and its entry together.

Two markers, and only two:
**⚠** a trap that bites the next time you touch this ·
**⛔** decided, do not re-open (the reason is in DECISIONS.md).

## Environment
- Windows, drive D:\E-Lotto\elotto
- ESP-IDF at C:\esp\v6.0.1\esp-idf  (Tools: C:\Espressif)
- VS Code with Espressif ESP-IDF Extension
- Target: **esp32p4**
- Build system: idf.py via ESP-IDF Extension

## Concept
Four-node ESP32-P4 array. The master scores lottery numbers via GCP methodology; up to three slaves
measure the same window in parallel, triggered by one UDP broadcast (port 5000). Slaves are
discovered by broadcast at every session start — no IP table, no node count configured.

Combined z = **Σ z_node / √k** over the k nodes that answered *that* run, so a missing reply costs
that run's gain, not the session.

**Entropy is photons, and only photons** (user decision). Each node has its own OV5647, never
shared. Entropy = non-overlapping frame pairs, diff = f[2k+1]−f[2k] per pixel (cancels FPN exactly),
LSB packed, XOR-folded. ~5,7 Mbit/s per node idle, ~3,7 under load `[D25]`.
⛔ The on-chip TRNG is deleted from both firmwares — a whitened hardware RNG would be
indistinguishable from the real thing in every statistic this project computes. The Fisher–Yates
order uses an xorshift32 PRNG seeded from the camera; it never enters a z.

**The ×√n gain is NOT established** — it assumes node independence. Judge a session on per-block
combined σ **and** the full pairwise matrix, never on `pair_r` alone: the 08-13 pass had σ 1,378 with
every pairwise |r| ≤ 0,024. **σ, not correlation, is where this array fails.**

Modes: Eurojackpot (5 of 50 + 2 of 12, 7920 combinations) and 6 of 49 (5005).
⚠ `?mode=` is `1` for 6-of-49 and **anything else** for Eurojackpot — it tests `val[0]=='1'`, so
`?mode=649` silently starts a Eurojackpot session.

---

## v3 — the single-pass session
**PLAN.md §2 is the contract.** Every combination in the confirmed pool is measured **exactly once**,
in one Fisher–Yates random order, with **one continuous window per Focus item** — scoring, baseline
and measurement share the same length.

- **No loops, no Runs cap, no ranking modes.** `?loops=`, `?runs=`, `?rank=` answer **400**. 100 % of
  the progress bar is the full combination space (Euro 12+5 → 7920; 6-of-49 pool 15 → 5005;
  `NUM_RUNS` 8000 is the hard cap). Unlimited Mode below is the one sanctioned exception.
- **Measuring time is a session parameter.** `?run=<s>` is **1–5 s, default 5** (user, 2026-08-18);
  out of range answers **400** rather than falling back. `?gap=<s>` defaults to 40 % of run. The
  segment count follows from `RUN_SEGS_REF`/`RUN_MS_REF` in `sensor.h`.
  ⚠ The requested window is not the wall time you get — actual is `focus_win_ms`, set by the
  **slowest** node's bit rate `[D2]`.
- **`results[]` is in MEASUREMENT order** (`.index` = combination id, `.block` stamped), so the
  prefix is always complete: aborts need no compaction, and `GET /results.csv?all=1` streams every
  measured item live mid-session.
  ⚠ RAM only. Pull it periodically over a long pass; a master reboot loses unrepeatable measurements.
  ⚠ Bare `/results.csv` is the 15-row summary, **not** the record.
- **Blocks are the statistics unit.** Every `cal_interval_ms` (default 15 min, `?calint=`, 0 = none)
  the pass parks for sweep + baseline together; the boundary closes a block → `/loops` row, drift
  point, pairwise fold, block centring.
- Pause stops the clock; Abort publishes the measured prefix.

### Phases
**Phase 0 — scoring.** Each number 1..N gets **exactly one long run** (the session window) in a fresh
Fisher–Yates order. Direction is pre-registered: `?score=high|low|abs`, default `high`.
⛔ Never repeat a target in place, in any form `[D5]`.

**Phase 1 — baseline**, all nodes in parallel, repeated at every block insertion. **The subtraction is
gone, not just inert**: the baseline is purely the drift reference (`LoopStat.base`), the independent
cross-check against the block's own master mean `raw_m` `[D36]`. Default 10 runs (`?baseline=`). The
UI bar says "Baseline — drift reference"; the `cal*` element ids are historical.

**Phase 2 — the pass.** One Fisher–Yates order over the whole combination space, each item measured
once at the same window as every other phase. `PairAcc[i][j]` accumulates per-block-centred moments
for every node pair over runs where both contributed → full Pearson matrix + per-node σ in `/status`
(flagged if |r|·√n > 3). `close_block()` → `record_loop()` stores per-block offsets/σ and camera
health; `drift_add()` regresses the master's per-block mean on the block index → `drift_slope`,
`drift_t`; |t| > 3 flags real drift.

### Two attended gates, opt-in on `POST /start?confirm=1`
The web UI always sends it; curl never does.

- **`PHASE_READY` — the observer gate.** Parks after calibration and baseline, released by
  `POST /ready`, then holds 1 s dark (`READY_SETTLE_MS`) before the first number. No timeout, by
  design. ⚠ Armed on `focus_mode`, not on `confirm` alone `[D6]`.
- **`PHASE_POOL_CONFIRM`.** `POST /pool?act=ok|more|cancel&main=..&euro=..`. "Select more" re-scores
  with the still-checked numbers omitted, so they keep the measurement that chose them. Keeping
  exactly 5+2 gives ONE combination — intended, and the highest-power way to use the instrument.
  15-minute timeout accepts unchanged and records `pool_auto=1`; at `focus=0` it accepts immediately
  and records the same flag.

### Unlimited Mode — rounds instead of one pass (2026-08-18)
UI checkbox "∞ Unlimited mode" + **Runs per round** (default 100); `POST /start?unlimited=1&maxruns=<n>`.
Bare `?runs=` still answers 400, now naming `unlimited=1&maxruns=` instead.

A **round** = score every number → keep as many of the best as fit `maxruns` runs → measure that
space once in a fresh Fisher–Yates order → score again. It ends only on **Abort** or `results[]` full
at `NUM_RUNS` 8000 — and since 2026-08-19 the buffer compacts rather than filling, so in practice
only Abort ends it. The stop survives as the backstop for a compaction that cannot allocate.

- **Pool sizing maximises COMBINATIONS MEASURED, nothing else** (`unlimited_pool_sizes()`). The
  bonus-number preference survives only as the tie-break: equal combination count → larger bonus
  pool, then larger main pool `[D3]`.
  cap 100 → Euro 7+3 = 63, 6-of-49 9 numbers = 84 · cap 500 → Euro 11+2 = 462, 6-of-49 11 = 462 ·
  cap 7920 → the full space, i.e. repeated full passes.
- **Results ACCUMULATE.** `results[]` is never cleared between rounds; every table, the pass
  statistics and the Bonferroni line run on the union of all rounds so far.
- **A full buffer compacts instead of ending the session** `[D42]`. When the next round would not
  fit, the round boundary folds everything except the best, worst and most ordinary
  `PASS_KEEP_PER_TABLE` (16) items into moments and drops the rest. Pass mean/σ/χ²/Bonferroni stay
  exact over every item measured; Top-N and Bottom-N stay exact outright.
  ⚠ Only when it must, so a session that fits keeps its complete archive.
  ⚠ `completed` in `/status` is `items_done` (monotone). `runs_completed` is ROWS HELD and steps
  back at a compaction — never report progress from it. **`items_done` is not an index**; anything
  addressing `results[]` takes `runs_completed`. Getting that wrong is what broke the first
  compaction on hardware `[D42]`; it shows as `pass_n_valid` ≠ `completed`, not as a bad σ.
  ⚠ The round has **two** bases and they are not interchangeable: `round_base` is the results[] index,
  `round_item_base` the items_done value. A progress figure or the CSV `items=` field takes the item
  one — mixing them printed "item 16184 / 378" once compaction had moved them apart.
  ⚠ `compacted=` in the CSV header says what the file is NOT: non-zero means the rows are the
  extremes plus survivors, not a sample. Never compute a distribution from them.
- **Every round closes its own block**, even at `?calint=0`, so centring never mixes items from
  either side of a re-scoring. Rounds after the first re-run sweep + baseline **before** scoring
  (skipped at `?calint=0`): the scoring runs choose the pool and must not sit on a stale sweep.
- **No pool-confirmation gate** (choosing by score is what the mode is); recorded `pool_auto=1`. The
  observer gate still fires once at session start when attended.
- ⚠ **Inside a round "measured exactly once" holds; across rounds a combination can recur**, because
  a later scoring pass may pick overlapping numbers. Each recurrence is its own row — nothing is
  averaged. `index` is meaningful only within a round; the identity is **(round, index)**, and
  `n1..n6;e1;e2` is unambiguous either way.
- **Pooling**: see **⚠ Pooling** below — unlimited data carries two extra conditions.
- A truncated round draws a **random subset** (partial Fisher–Yates over the whole space). Only the
  last round truncates `[D4]`.
- `/status`: `unlimited`, `runs_cap`, `round`, `round_base`, `round_done`, `round_total`. `completed`
  is session-wide, `total` is the CURRENT round — ⚠ a progress bar must use `round_done/round_total`
  or its denominator moves. ETA is to the end of the round.
- `/status` publishes `pool_main`/`pool_euro` for the whole of every running session. ⚠ `sensor.c`
  withdraws them at the ROUND BOUNDARY (`round++`) and at session start before `state` goes RUNNING —
  clearing at the scoring pass would leave the previous round's numbers on screen through the whole
  insertion, and the previous SESSION's through the opening sweep and observer gate.
- The **Runs per round** field shows what each budget buys and how long a round takes:
  `Euro 7+3 = 63 runs · ≈ 19 min/round`. Model in `sensor.h`:
  **`cycle_ms ≈ 200 · segments / rate + CYCLE_FIXED_MS + gap_ms`**, rate = slowest node's `cam_mbit`
  from `/status`, else `CYCLE_LOAD_MBIT_X100` (3,66 Mbit/s under load) as cold start `[D39]`. A round
  = scoring sweep + the pool's combinations + one insertion + one more per `calint` of MEASURING time.
  An estimate; the live ETA uses measured pace.

Verified on hardware 2026-08-18: three rounds at `maxruns=20` 6-of-49, pools re-scored between rounds,
both blocks centred to `pass_mean` −0,000000, abort kept the prefix; Eurojackpot at `maxruns=50` sizes
to 6+4 = 36.
**Not yet exercised:** a long run with real 15-minute blocks, and the `results[]`-full stop at 8000.

---

### Spectral entropy — the second channel (2026-08-25)
**z is the DC bin.** `Σ(ones−100)` is exactly the 0-th Fourier component of the segment series, so a
statistic built from bins 1…511 is independent of z under H₀ — a second measurement out of the same
bits, at no extra measuring time. Welch over `GCP_SPEC_W` 1024-segment windows, `GCP_SPEC_BINS` 511
(DC dropped because it is z; Nyquist dropped because it is χ²₁, not χ²₂), normalised entropy H/ln K.

- **`?went=<0..1>` is the weight of the entropy half of the ranking key**, default **0,50** (user,
  "wir probieren mal 50:50"). Out of range answers **400**. `?went=0` is the control arm and
  reproduces the pure-z ranking exactly, including the scale.
  **key = ((1−w)·z_ctr − w·z_h)/√((1−w)²+w²)** — minus z_h because **low entropy is the interesting
  direction**, and the √ normaliser because both halves are N(0,1) and independent, so the key stays
  unit-variance at every w. Direction-neutral: `?score=` still only picks the pool.
- ⚠ **It RANKS, it does not TEST.** Top-5/Bottom-5/Nearest and the compaction survivors run on
  `rank_key()`; `pass_mean/σ/χ²`, the Bonferroni line and `null_flags` stay on `rank_z()` — the
  closed-form entropy null is the IDEAL one and this array does not quite meet it (1600 consecutive
  segments share a frame pair, so per-frame structure is a real spectral line). Block centring
  removes that as the constant it is; a p-value from it would not be honest.
- **Null in closed form, from the run's own M** (`gcp_spec_null()`): H₀ = 1 − (ψ(M+1)−ln M)/ln K,
  Var = Var(Z)/(K·M²). Verified against Monte Carlo: mean to 8e-6, σ 1,6 % (M=25) / 5,5 % (M=127)
  conservative. M is fixed per session, so the σ error is a constant scale and cannot reorder.
  **1 s works**: M ≈ 25, σ(H_norm) ≈ 2,0e-4 · 5 s: M ≈ 127, σ ≈ 3,9e-5 — the same statistic, 5×
  sharper, directly comparable because z_h is standardised per run.
- **z_h is block-centred per node like z**, on its own accumulator (`s_hacc`) and its own k — a node
  can answer with a z and no H, so `k_h ≤ k`. It needs centring more than z does: the per-node offset
  follows the exposure rung, hence the frame cadence, hence how much per-frame structure lands in the
  spectrum. ⚠ Same pre-registration cost, and it bites harder: an entropy effect **constant across a
  block** is removed with the offset.
- `ENT_Z_CLAMP` 12 bounds the entropy term **in the key only** — the archive is never clamped. One
  stuck frame would otherwise own Top-5 for the session. Hits are counted (`ent_clamped`); non-zero
  means camera glitches were competing for the table.
- Wire: `Z:<z>,<H_norm>` — **appended and optional**, 8 decimals (6 would quantise z_h at `?run=5`).
  M does not travel: every node measures the commanded `nseg`, so M = nseg/1024 on all of them.
  A slave without the field contributes to z and not to entropy.
- **`GET /spectest`** holds the packed real FFT against a reference DFT and prints the null moments
  at the session's window count. **Master only**, like `/camtest`, and for the same reason: the test
  is pure arithmetic out of the shared component, so all four nodes run the identical object code —
  a per-node run would answer a question that cannot differ. 409 while measuring.
  ⛔ Do not change `spec_fold()` without it — a wrong unpack gives a wrong-but-plausible H.
  Verified on hardware 2026-08-25: `worst_rel` 2,8e-06, `h0` 0,99936953, `sd` 3,939e-05 at 127
  windows — the target's digamma agrees with the Python reference to every printed digit.
- **The archive carries z_h even at `?went=0`.** The control arm measures, combines and centres the
  entropy exactly as the 0,5 arm does; the weight only enters the ranking. So a `?went=0` session can
  be re-ranked at any weight offline from `h0..h3` in the CSV, and the two arms differ in nothing but
  the order of three tables.
- ⚠ **`ent_w` splits the pooling table for the TABLES only.** Two sessions at different weights put
  different items in Top-5. `z_raw`/`z_ctr` still pool — the entropy channel does not touch them.
- ⛔ **The FFT working buffers go in INTERNAL RAM, never PSRAM** — measured, and the margin is not
  subtle. The first build put all ~12 KB in PSRAM and the slaves ran at **3,81 Mbit/s against 5,72**
  for a node still on the pre-FFT image, `ms_extract` 67 ms against 39 — a third of the bit rate
  gone. It is not the arithmetic (25 windows × 512 points is ~750 kFLOP per 1 s run); it is the
  **bus**, shared with the capture buffers and the extraction ring. Moving them internal restored all
  four nodes to 5,716–5,717 and `ms_extract` 39,5–39,8, i.e. no measurable cost at all.
  `gcp_spec_in_psram()` / `"in_psram"` in `/spectest` reports the fallback, because a node that lands
  there is merely slow and nothing else would say why.
  ⚠ **Never judge this on the master's rate**: the master finishes its run and then waits ~1,3 s for
  the slave replies, so its extraction task gets the idle bus back and it read 5,717 in BOTH builds.
  The slaves are the measurement.
- ⚠ **`?went=0` does not disable the FFT.** Entropy is always measured, combined and centred; the
  weight only enters the ranking. The control for a *cost* question is therefore the pre-FFT image,
  not `?went=0` — a within-session comparison against one node left on the old firmware is the
  method that worked.

## Stored z is RAW; ranking is block-centred
- **`z_score` is the raw combined Stouffer z and is never rewritten.** It is the archive.
  `/results.csv` carries it forever, alongside the per-node `z0..z3`.
- **`z_ctr` is what every statistic and every ranking runs on.** At block close `center_block()`
  subtracts each node's own block mean and recombines over the same nodes (`have_mask`, so k and the
  √k scaling are unchanged). Pass mean/σ/χ², Top-N, Bottom-N, nearest-zero and the Bonferroni line all
  read it through the single accessor `rank_z()`. Until a block closes, `z_ctr` holds the provisional
  raw value. `[D8]`

⚠ **Centring removes any real effect CONSTANT across a whole block.** Pre-registration decision, not a
detail: what remains visible is an effect varying **between items inside a block**.

### UI
- A **parameter line** at the top of the progress area, for every session — mode, measuring time, gap,
  segments, s/run, measured window/gap, baseline runs, sweep budget + interval, score direction,
  attended/unattended, unlimited cap. Built from `/status`, not the form, so a curl-started run and a
  reloaded page both label themselves. "per run" is measured pace once ≥ 5 runs exist, else the rate
  model, and it says which. ⚠ The measured value reads high early — `elapsed_ms` also contains the
  opening sweep, which is not a run.
- **Three tables of five**: Top-5, Bottom-5, Nearest-zero-5 (`top[]`/`low[]`/`near[]`), plus
  significance line, item counter + block badge, Save CSV.
  ⚠ **Nearest zero means nearest the PASS MEAN**, not nearest raw 0 `[D9]`. The table shows `Z*`;
  `/status` publishes `pass_mean`/`pass_sigma` so the choice is checkable.
  The studentized-view checkbox of PLAN.md §2.3 was never built — the `Z*` column is all there is `[D10]`.
- CSV is **German**: `;` separator, `,` decimal — a decimal point makes Excel read the column as text.

### CSV header
`# elotto v3 mode= focus= score= items=<measured>/<planned> ranked= excl= void= blocks= paused_ms=
pass_* v_eff= null_flags= flush_timeouts= drift_t= unlimited= runs_cap= rounds= run_s= run_segs=
gap_s= compacted= ent_w= ent_win= ent_h0= ent_sd= ent_n= ent_clamp= rank_mean= rank_sigma=
fw=<version>/<elf sha>`, then `# nodes=<ip list, discovery order>` and
`# fw_nodes=<sha per node, same order>` (`?` = never answered).
`?all=1` appends a **`round` column last**, then `zh_ctr;key;h0..h3` after it, so older parsers
still line up. The summary file gains `zh_ctr;key` at the end of its row. `h0..h3` are per-node
RAW z_h in the same discovery order as `z0..z3`; empty means that node reported no H, which is
**not** the same as an H of zero.
⚠ The window travels in BOTH units: `run_s` alone cannot separate instrument generations `[D1]`.
⚠ "All four run the same code" is a policy, not a fact — check `fw_nodes`.

---

## ⚠ Pooling — the complete list
Two sessions may be pooled only when every line below holds. Each is a separate instrument or a
separate arm `[D1]`.

| split on | pool only within |
|---|---|
| hardware change 2026-07-29 | after it (`docs/data/` holds only post-07-29 sessions) |
| block centring 2026-08-13 | one side, or recompute both the same way |
| extraction speed-up 2026-08-18 | one side — same `?run=`, 1,85× the bits per item |
| onset flush 2026-08-19 | one side — the bit-to-item mapping changed |
| `focus=on` vs `off` | one arm; attended and unattended are never mixed |
| `ent_w` (2026-08-25) | one weight — for the TABLES. `z_raw`/`z_ctr` pool across weights |
| v3 vs any v2.x | v3 only |

Unlimited-mode data carries two more: split on `round` before pooling with a single-pass session, and
decide what to do about combinations that recur across rounds before pooling rounds together.

---

## Pass-level null gates and node health
Under H₀ with a working instrument: mean ≈ 0, σ ≈ 1, Σz² ≈ n. **Ranking is secondary; when
`null_flags` ≠ 0 the extremes are not decisive.** Bits: `SIGMA` 0x01, `DRIFT` 0x02, `CHI2` 0x04,
`PAIR` 0x08, in `/status`.

**Soft-down** (`nodes[].soft_down`) takes a node out of the combine after a block with
σ > `NODE_SIGMA_SOFT` (1,25). Sticky; clears after `NODE_SOFT_CLEAR_BLOCKS` (4) blocks with
σ ≤ `clear_sig` = peer median × `NODE_SOFT_CLEAR_SIG_K`, floored at `NODE_SOFT_CLEAR_SIG`, capped
below the trip bar, published per block in `/loops`. It never reboots anything.

- **σ is the only trip criterion.** |mean| over `NODE_MEAN_REPORT` is a flag (`mflag` in `/loops`),
  never an exclusion: centring removes a constant block offset from everything ranked, and the
  offsets come from the exposure rung `[D11]`.
- **The clear bar is peer-referenced because a constant was the array's own median** `[D12]`.
  ⚠ Replay a threshold change against `/loops` before believing it.
- `NODE_SOFT_MIN_COMBINE` is **1**, so up to three of four may drop out and a solo combine is
  possible `[D13]`. `k` is in the CSV per item.
- **Quarantine**: a block a tripping node could have contaminated is excluded from ranking
  (`skip_rank=1`) but stays in the CSV. Fires on every trip, but only when that node was actually in
  the block's combine `[D14]`.
- A soft-down arm still writes its z to the archive, so any exclusion can be undone offline `[D41]`.

**A node whose camera stalls is REPORTED, DROPPED and REBOOTED** — there is nothing to fall back to by
design. The node replies `E:<reason>`; the master names it in `fault`, drops it, bumps
`nodes[].reboots` and sends `R`. It rejoins the *next* session by discovery. The session aborts
(`src_stalled`) only if the drop would leave fewer than two nodes.
⚠ **The master never reboots itself** on its own camera failure — that would destroy the `/loops`
history and the operator's results. It faults, reports, aborts.

A run that dies part-way produces **no z at all**: `gcp_zscore_raw()` returns false rather than a
short run, whose z would be normalised by a √segments it never reached. Archived with `k=0` (VOID),
never ranked.

---

## Focus display
A "Focus:" card shows the current target in large type — the candidate number while scoring, the whole
draw while measuring — so the observer is present while the noise is sampled (the original GCP/PEAR
protocol). It changes nothing statistically, so a session is merely **tagged**: `/start?focus=1`,
`"focus"` in `/status`, `# focus=on|off` in the CSV.

- The panel lights ~70 ms **before** the bits start, by design ⛔ `[D33]`.
- `GET /focus` (~60 B) is polled at 10 Hz, deliberately separate from the 2,5 KB `/status`: `seq` is
  monotonic per window, so the UI counts *missed* windows — a skipped window credits an effect to the
  wrong combination, which is mislabeling, not blur.
- `POST /pause?on=1|0` holds **between** runs only; state stays `running` and paused time is excluded
  from `elapsed_ms`.
- **Every window starts on fresh bits, attended or not** (`onset_settle()`, `ONSET_SETTLE_MS` 500 ms
  cap), on all four nodes — the slaves flush on `M`, so no wire command was needed `[D34]`.
  ⚠ The trigger goes out BEFORE the master settles, so all four flush in parallel.
  ⚠ A flush that does not finish **voids the run** (`flush_timeouts` in `/status` and the CSV) `[D35]`.

---

## Camera calibration (per BLOCK)
At every insertion the master broadcasts `K<budget_ms>,<segs>`, sweeps its own ladder in parallel, and
waits for every node's `OK:<exp>,<gain>,<fold>,<bias>,<mbit_s>,<G|U>`. Each node keeps the setting with
the **lowest |bias − 0,5| among candidates clearing the σ gate with margin** (|σ−1| ≤ half the
tolerance), falling back to the bare gate if none qualify `[D16]`.

Gates a rung must clear: bias (run-scaled, see below), autocorr < `CAL_AUTOC_TOL` 0,01, |σ−1| ≤ 0,05,
no stuck frames, `mean_px` in **[`CAL_MIN_MEAN_PX` 5,0 , `CAL_MAX_MEAN_PX` 100,0]** `[D18]``[D20]`, and
`zero_diff` ≤ `CAL_MAX_ZERO_DIFF` 0,125.

- **The dark end is gated because photons do the whitening** `[D18]`. Exposures 4 and 8 are rejected
  on this rig; 16…128 pass. Dim the lamp and more rungs start failing; the answer is light, not a lower floor.
- **The bias bar is a z offset**, `CAL_MAX_Z_OFFSET` 1,0, converted with the session's segment count
  via `gcp_z_per_bias()` — which is why the count travels on `K`. Never tighter than
  `CAL_BIAS_SE_K`×SE(bias), never looser than the old 1e-3 `[D19]`.
  At a 10 s budget it resolves ~6,9e-4, against the 2,3e-4 the health bar corresponds to — the gate
    is bounded by its own sampling error, not by the number in it.
- ⛔ **No fold trial.** The XOR fold is permanently on `[D17]`.
- ⚠ **Autocorrelation must not be subsampled** — `autocorr_max` is a gate, so a noisier estimator
  would change which rung is chosen.
- The budget is a **cap, not a target**: never estimate progress against it. Default 10 s, `?cal=<ms>`,
  `?cal=0` turns it off `[D21]`.
- **The trigger is TIME**, default 15 min (`?calint=`), 0 = no mid-pass insertions. Thermal drift moves
  on wall-clock time, and the cadence also sets the drift regression's resolution and the block size
  centring estimates its means from.
- **Nodes land on different exposures on purpose** (different sensors, different light). What they must
  share is the segment count.
- `GET /calibrate` serves the whole last sweep per candidate with the gate each failed, **on every
  node** — which is what makes a per-node optical fault diagnosable. The chosen setting is recorded per
  block in `/loops`; a re-tune nobody logged is indistinguishable from drift in the data.
- ⚠ `camera_get_stats()` is cumulative **since the last `camera_stats_reset()`**, i.e. since the last
  sweep. In a `?cal=0` session there is no reset and they are lifetime averages.

---

## Illumination — standing rules
The enclosure is **LIT, not dark** `[D28]`.

- ⛔ **Never power illumination from a node's VSYS pin** — conducted PWM noise on the sensor's analog
  rail certified 0 of 9 rungs `[D29]`.
- ⚠ **After physical work, let the light settle ~30 min** before a long run `[D30]`.
- ⚠ **Do not judge the light by one `mean_px` reading** — sweep, or take a time series.
- ⛔ **All four nodes on PoE, permanently** (user decision) `[D31]`.
- ⛔ **Do not switch camera hardware** on the theory that the OV5647 is the problem `[D32]`.

---

## Project structure
- **main/elotto.c** – app_main, Ethernet, webserver, HTML/JS UI. Endpoints: `/` `/status` `/start`
  `/abort` `/loops` `/results.csv` `/focus` `/pause` `/calibrate` `/pool` `/ready` `/probe` `/expose`,
  plus `/diag` (four-camera health page) and `/diagjson` (the master's own stats; slaves serve the same
  JSON at their `/diag`).
  `/loops` is per-**block** health and carries the block's own exclusion verdict: `clear_sig`, `quar`,
  and `soft`/`trip`/`mflag` per node.
  `POST /expose?exp=<lines>[&gain=<g>]` sets one node's operating point by hand, served by every node
  for its own camera; driven from `/diag`'s −/+ buttons. Resets the camera statistics so `mean_px`
  answers in ~2 s — the point is tuning the physical LIGHT against a live reading. 409 while measuring;
  the reply carries the **read-back** setting. Not sticky: the next sweep overwrites it, which is correct.
  ⚠ `cfg.max_uri_handlers` must exceed (endpoints here) + 5 from elotto_ota. Registration past the cap
  fails and **the return value is checked nowhere**, so an endpoint just 404s silently. Currently
  17 + 5 = 22 against a cap of 24 (`/spectest` added 2026-08-25).
- **main/sensor.c** – GCP analysis, scoring/pooling, baseline, the pass, blocks, centring, drift,
  soft-down, publishing. **main/sensor.h** – types and declarations.
- **main/nodes.c** – the array: UDP link, discovery, calibration handshake, per-node health, drop/reboot
  policy. `nodes.h` is the API; sensor.c reaches other boards only through it.
- **main/focus.c** – focus panel, pause, run gap, session clock. One file because they share state:
  `pause_gate()` accumulates held time, `elapsed_ms_now()` subtracts it, a pause nudges the gap timer.
- **partitions.csv** – shared table (factory 1 MB + ota_0/ota_1 3 MB on 32 MB flash). A board flashed by
  one project must be updatable by the others.
- **ota_firmware/** – the network updater, its own IDF project. Ethernet + HTTP + esp_ota only.
- **components/elotto_camera/** – OV5647 entropy extraction (camera.c, extract.c, include/camera.h).
- **components/elotto_link/** – the UDP wire format (`EL1 <seq> <payload>`, ports 5000/5001), plus
  `EL_SEG_MIN`/`EL_SEG_MAX` as ONE definition for both firmwares.
- **components/elotto_gcp/** – the z-score primitive (`gcp_zscore_raw()`) and `gcp_z_per_bias()`.
  Shared so the nodes cannot disagree about what a z is `[D37]`.
  ⚠ `GCP_SEGMENT_SD` is the literal `7.07106781`, not `sqrt(50.0)`.
  ⚠ The remaining soft-float calls per segment are deliberate `[D26]`; only `__subdf3` was removable
    without moving a stored z.
  `/camtest` checks `cam_popcount32` against `__builtin_popcount` over 200.000 values.
  Plus `gcp_spec.c`: the Welch spectral-entropy channel (float32 packed real FFT, closed-form
  null). `/spectest` holds it against a reference DFT — see **Spectral entropy** above.
- **components/elotto_ota/** – update endpoint + boot safety (rollback, boot counter, mark-valid;
  `/update` `/boot` `/reboot` `/poison` `/otainfo`). `BOOT_FAIL_LIMIT` 3, `HEALTHY_UPTIME_MS` 30000.

All three components are **shared**: the slave repo pulls them via
`EXTRA_COMPONENT_DIRS=../elotto/components`, and ota_firmware pulls *only* elotto_ota by pointing at
that single directory — IDF compiles every component it discovers, so pointing at `components/` would
drag the camera into the recovery image. ⚠ The repos must stay siblings on disk; build, flash and
commit them together.

### Wire protocol
`P` discovery · `B<runs>,<seg>` baseline · `M<seg>` measure · `K<budget_ms>,<segs>` calibrate ·
`D` diagnostics · `A` abort · `R` reboot. Replies `OK`, `Z:<z>[,<H_norm>]`, `D:`, `E:<reason>`,
`V:<reason>` (void, not a fault).

UDP loss is handled explicitly: every frame carries the sequence number it answers, mismatches are
dropped and counted (`net_stale`), and a timed-out command is resent under the same sequence so a node
replies from a one-entry cache instead of measuring twice.
⚠ **All receive timeouts go through `link_arm_timeout()`** — lwIP rounds `SO_RCVTIMEO` to whole ms and
treats 0 as *wait forever*, so a sub-millisecond remainder would hang the session. This happened once.
⚠ **A receiver does not clamp an out-of-range segment count**, it substitutes its own `[D38]`.
`LINK_MEAS_MS_FOR(nseg)` scales with the run and is deliberately generous `[D7]`.

### Resources
- **PSRAM is mandatory** with the camera: capture buffers, extraction ring, `loop_hist`, and the
  per-item per-node z archive `s_node_z` (~128 KB). Internal RAM is full with `results[]` — a few KB
  more of .bss fails the *link*, not the run.
- ⚠ **Task priority is load-bearing**: the extraction task (`ELOTTO_CAM_TASK_PRIO` = 4) is CPU-hungry,
  so any task calling `camera_read_word()` must run **above** it, or the consumer starves (~10×
  slowdown; signature is ring `drops` huge with `waits == 0`).

---

## Nodes
| node | IP | MAC | COM | flash contents |
|------|----|-----|-----|----------------|
| master | 192.168.178.100 | 80:f1:b2:d2:e3:1d | COM4 | factory = updater, ota_0/ota_1 = elotto app |
| slave0 | 192.168.178.103 (static lease) | 80:f1:b2:d2:e3:e5 | — | factory = updater, ota_0 = slave app |
| slave1 | 192.168.178.145 (static lease) | e8:f6:0a:e0:ce:a8 | — | factory = updater, ota_0/ota_1 = slave app |
| slave2 | 192.168.178.155 | e8:f6:0a:e0:c7:a1 | — | factory = updater, ota_0 = slave app |

**Say master/slave0/slave1/slave2, never the IP ending.**
⚠ **Column order in a results CSV is DISCOVERY order**, not the slave number, and it changes between
sessions. The IP list in the header is what makes `z0..z3` decodable — never map by column position.
⚠ **COM ports are not stable** — the same slave has enumerated as COM6, COM8 and COM9. List the ports
before an `erase-flash`; a wrong port wipes a working node.
Node addresses are informational: the master finds slaves by UDP broadcast, so a dynamic lease works
like a static one.

### Extraction — where the rate stands
**5,71 Mbit/s per node idle** (98,5 % of what the sensor can deliver `[D23]`), **~3,7 under measurement
load**, where the loop is compute-bound instead `[D25]`. The path from 3,42 to 5,71 is `[D22]`.

- ⛔ Nothing done to the extraction path can raise the **idle** rate — the second-core split stays
  dropped for a measured reason `[D23]`.
- The loaded rate is a different question and is still open `[D25]`. Prove any change with
  `ms_extract` under load, never at idle.
- `/diagjson` publishes the per-pair split on every node: `ms_pair` = `ms_wait` (blocked in DQBUF) +
  `ms_extract` + `ms_rest`, the remainder being yield plus preemption.
- **`GET /camtest`** runs the byte-wise reference against the word-wise path over six cases on the
  node's own silicon. `cam_extract_ref()` stays compiled in as the definition of correct.
  ⛔ **Do not change the live extractor without it.** 409 while measuring; master only.
  ⚠ It benchmarks the caller's real frame size (2×640000 B) and runs while the capture task is
  extracting, so `ns_*` varies ~10 %; for live cost read `ms_extract` from `/diagjson`.

---

## Build, Flash, Monitor
**Build** — always through `build.ps1`, which sets the environment and forwards its arguments. Shell
state does not survive between tool calls, and the script must use the **VS Code extension's** venv
(`C:\Espressif\tools\python\v6.0.1\venv`), not `export.ps1`'s, or the build dir gets pinned to the
wrong interpreter and fails with "run 'idf.py fullclean'".

```powershell
.\build.ps1 build                        # master
.\build.ps1 -C ota_firmware build        # updater  (-C selects another project)
.\build.ps1 -C ../elotto_slave build     # slave
```

**Flash over Ethernet, not USB:**

```powershell
curl http://192.168.178.100/update --data-binary @build/elotto.bin                        # master
curl http://192.168.178.103/update --data-binary @../elotto_slave/build/elotto_slave.bin  # slave
```

~750 KB in ~3 s. The node writes the *inactive* slot, reboots, and marks itself valid only once its
webserver answers, so a failed transfer cannot strand it. `POST /update?slot=0|1` targets a slot;
`POST /boot?slot=N` selects the next boot slot and clears the fail counter — that is how a node is put
back on a known-good image without USB.

- `/update` and `POST /start` return **409** while a session runs. Abort first.
- ⚠ **After every OTA, poll `fw_sha` in `/status` until it CHANGES** `[D27]`.
- ⚠ **A node that pings but refuses port 80 is not dead** — check `/otainfo` before reaching for USB
  `[D40]`.
- **USB is only for** a fresh board (bootloader + partition table + factory updater) or a node whose
  recovery updater is gone: `.\build.ps1 -C ota_firmware -p COMx erase-flash`, then `... -p COMx flash`.

| Action | VS Code shortcut |
|---|---|
| Build only | Ctrl+Shift+B |
| Menuconfig | Ctrl+E G |

## Rules
- ⚠ **Never edit sdkconfig manually.** Edit `sdkconfig.defaults`, delete `sdkconfig`, let the build
  regenerate it, verify the diff. Every project has a `sdkconfig.defaults` and sets `IDF_TARGET` in its
  CMakeLists — without both, a regenerate loses settings or fails with "CMAKE_C_COMPILER not set".
- Target is always esp32p4.
- ⚠ The slave's `sdkconfig.defaults` must keep `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` **before**
  `ESP32P4_REV_MIN_0=y` — the latter depends on it, and without it the choice silently falls back to
  rev v3.1 and the binary refuses to boot on these v1.3 boards.
- Firmware is delivered **over OTA only** in normal workflow; `build.ps1` documents build, not serial
  flash.

---

## Where things stand (2026-08-20)

**The instrument is sound and every result so far is null**, which is what a working null instrument
produces.

**Hardware:** all four nodes lit and healthy at idle — bias within 1,4e-4 of 0,5, σ 0,996–1,003,
autocorr ≈ 0, 5,71 Mbit/s each. Settled light at exp 128: master 34,5 · slave0 27,9 · slave1 27,0 ·
slave2 19,5, stable to ±0,6 % over 18 min.

**Flashed 2026-08-20** — master `703e98b24897e10f`, all three slaves `3c53a5e1ff7adff0` (unchanged;
the slave sources did not move). The 08-19 checks still hold: all four certify a rung and land on
**exp 128/64/64/64**, none on 4 or 8; `clear_sig`, `quar`, per-node `soft`/`trip`/`mflag` in `/loops`;
`# fw_nodes=` in a fresh CSV.
⚠ The 2026-08-20 history rewrite renumbered every commit from `18074b5` on. **Match a pre-08-20 build
by ELF SHA, not by the version string.**
⚠ **Not exercised on hardware**: `mflag` firing (needs a real |mean| > 1,5 excursion, and the gated
rungs are what used to produce them), and the `round_base` fix — compaction itself has now run, see
below.
⚠ Flash master and all three slaves **together**: `K` carries a field and `D` another. Both
mismatches degrade safely (legacy bias bar / no sha in the header), but a half-flashed array is not
one instrument.

**The 2026-08-20 session, and what it cost.** 8019 items, 18 rounds, 4 h 09 min, aborted at ~04:03
when all three slaves missed `NODE_MISS_LIMIT` inside the same ~50 s. Every node was healthy
afterwards; the FritzBox logged a DNS fault at 04:09 that this array cannot even use, since it
resolves no names. The master now records which side went quiet — `eth_up`, `eth_downs`,
`eth_lost_ips`, `drop_*` and `uptime_ms` in `/status` — so the next one is answerable without
reconstructing the abort time from slave camera counters.
⚠ **The session's own statistics are wrong** and it is not archived. Compaction fired for the first
time and `round_base` was taken from `items_done` where it had to come from `runs_completed`: the
round after the compaction wrote past the compacted array and the dropped rows were counted twice.
`pass_n_valid` 15806 for 8019 items. Fixed the same day `[D42]`.

**Spectral entropy went in 2026-08-25** (see the section above), flashed to all four and verified on
hardware the same day. Master `dcf44ad31c6e8e2d`, all three slaves `a0a5d77e03fcb942`.

What was checked: `/spectest` `worst_rel` 2,8e-06 and the null moments identical to the Python
reference at both window counts · `/camtest` still `equal:true`, so the z path is untouched · the key
reproduces by hand from the CSV columns (`(0,5·0,2771 − 0,5·(−0,8199))/√0,5 = 0,7757`, the published
value) · Top-5 ordered by the key with negative `zh_ctr` throughout · the optional wire field
degrading correctly, with one node deliberately left on the pre-FFT image writing an empty `h3` and a
`k_h` of 3 while still contributing its z · and the bit-rate control above.

**Still not exercised**: a long run with real 15-minute blocks, so the entropy channel has never been
BLOCK-CENTRED on more than a couple of blocks — every check so far ran at `?calint=0` on provisional
values. ⚠ Watch the per-node spread: in the first run the master sat at z_h ≈ −2,9 while the slaves
were near +1,7, which is the offset centring is there to remove and the reason it must be confirmed
over many blocks before any ranking is believed.

**Open, in the order I would pick them up:**
1. **Does an offset survive on a GATED rung?** The 08-19 blocks at exp ≥ 16 average −0,05…+0,09, but
   individual blocks still reach ±0,6 at an SE of 0,08 — real offsets, just no longer big enough to
   fire anything. Watch `mflag` in `/loops`. (What is closed: the master is not a bad arm, its
   exposure rung was `[D11]`.)
2. **Verify the centring on a full pass.** Verified on a short run (closed blocks at mean(z_ctr)
   exactly 0,0000); never on a full session.
3. **Does calibration reduce the RATE of bad blocks?** The control pair only asked "does it add
   variance?" (no). The tail question needs a count of excursions over many blocks, not a mean.

**Recently closed:** the master's block offsets `[D11]` · slave1's σ excess follows the board, not the
camera `[D15]` · why the last two extraction changes bought nothing `[D24]` · which side goes quiet
when nodes drop (`drop_*` in `/status`, 2026-08-20) · the `round_base` fix, verified on hardware in
the 08-20 Eurojackpot session at 48 rounds and `compacted` 15828: `pass_n_valid` == `completed`.
**Dropped and deferred:** see the last section of [docs/DECISIONS.md](docs/DECISIONS.md) — check it
before proposing anything that sounds obvious.

### Session archive
`docs/data/` holds the post-2026-07-29 sessions.

| directory | what |
|---|---|
| `2026-08-05_6of49_fullpass/` | 5005 items, ~14 h — ran with `cam_cal=0` on all four nodes |
| `2026-08-08_6of49_aborted3404/` | 3404 items + all four ladders |
| `2026-08-08_6of49_pool11/` | 462 items, complete, first pass on the results code |
| `2026-08-13_6of49_fullpass_unattended/` | 5005 items, uncentred, with the README that motivated centring |
| `2026-08-18_6of49_unlim_run1s/` | first unlimited-mode session |
| `2026-08-19_6of49_unlim_overnight/` | 29 blocks; the session that produced `[D11]`, `[D18]`, `[D19]` |
| `2026-08-19_6of49_unlim_full8000/` | 8000 items, 18 rounds, 11,6 h — hit the buffer stop; the replay set for `[D42]` |
| `_live_*` / `_short_*` | a complete 5005 pass (13,4 h, curl-started, no gates); a 1995/5005 partial |
| `_analyze_*.py` | the operator's own analysis scripts — they skip `#` header lines |
