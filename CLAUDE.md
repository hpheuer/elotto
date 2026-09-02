# elotto – ESP32-P4 Project

**Rules live here. Evidence lives in [docs/DECISIONS.md](docs/DECISIONS.md)**, cited as `[D<n>]`.
Change a rule and its entry together. **Module-local traps live as comments at the code site** —
this file keeps only what bites across files or from the operator's side.

Two markers, and only two:
**⚠** a trap that bites the next time you touch this ·
**⛔** decided, do not re-open (the reason is in DECISIONS.md).

## Environment
- Windows, drive D:\E-Lotto\elotto
- ESP-IDF at C:\esp\v6.0.1\esp-idf  (Tools: C:\Espressif)
- VS Code with Espressif ESP-IDF Extension

## Purpose
Rank lottery **numbers** (scoring → pool) and **combinations** (the pass) from camera photon noise.
Two channels, one key `[D65]`:

- **z** — binomial of the camera LSBs in that window (the old z_pre; there is no second z,
  and since `[D73]` no second field either)
- **conc** — half-window concordance of the same bits (same sign both halves, loudest node dropped)

`?wpre=` is the concordance weight (form 0,8; API 0 = z alone). Scoring **selects** the pool on
that key; the pass **ranks** items on the same key; the UI shows Z*, Z, Conc.

**How the cameras stay usable.** Frame-pair LSB — z is the camera bits. The **sweep**
picks the exposure with the lowest |bias−0,5| among rungs that still look like noise (autocorr,
relative σ, dark / zero_diff). If a node's block σ is too loud against its peers, **soft-down**
takes it out of the combine. The next sweep — at the next round boundary —
recalibrates every node including that one. It returns after four
clean blocks. ⚠ `?cal=0` turns every sweep off; a trip then waits until the next session.

## Concept
Four-node ESP32-P4 array. The master scores lottery numbers via GCP methodology; up to three slaves
measure the same window in parallel, triggered by one UDP broadcast (port 5000). Slaves are
discovered by broadcast at every session start — no IP table, no node count configured. Combined
z = **Σ z_node / √k** over the k nodes that answered *that* run, so a missing reply costs that
run's gain, not the session.

**Entropy is photons, and only photons** (user decision). One OV5647 per node, never shared.
Non-overlapping frame pairs, diff = f[2k+1]−f[2k] per pixel (cancels FPN exactly), LSB packed.
⛔ LSB bits as measured `[D65]`. ⛔ No on-chip TRNG (covers an LFSR fed from the camera). The Fisher–Yates
order uses an xorshift32 seeded from the camera; it never enters a z.

**The ×√n gain is NOT established** — it assumes node independence. Judge a session on per-block
combined σ **and** the full pairwise matrix, never on `pair_r` alone: **σ, not correlation, is
where this array fails** (measured: σ 1,378 with every pairwise |r| ≤ 0,024).

Modes: Eurojackpot (5 of 50 + 2 of 12, 7920 combinations) and 6 of 49 (5005).
⚠ `?mode=` is `1` for 6-of-49 and **anything else** for Eurojackpot — it tests `val[0]=='1'`, so
`?mode=649` silently starts a Eurojackpot session.

---

## v3 — rounds until Abort
**This file is the contract, as rounds `[D67]`.** (`docs/PLAN.md` is a stub.) A round scores every number, keeps as many of
the best as fit `?maxruns=` (default 100), measures that space once in a Fisher–Yates order,
then scores again. Ends on **Abort**. Inside a round each combination is measured exactly once;
across rounds a combination can recur — identity is **(round, index)**.

- **No loops, no Runs cap, no ranking modes.** Unknown start parameters answer **400**. 100 % of the progress bar is the full combination space. `NUM_RUNS` 7200 is the hard cap —
  ⚠ **Eurojackpot's 7920 does not fit uncompacted**; a full Euro pass compacts once near the end
  `[D45]`. Verify, do not assume.
- **Measuring time is a session parameter.** `?run=<s>` is **0,5–5 s, default 5**; out of range
  answers **400**, no fallback. `?gap=<s>` defaults to 40 % of run (floor `GAP_S_MIN` 0,5);
  segment count follows from `RUN_SEGS_REF`/`RUN_MS_REF` in `sensor.h`. ⚠ The requested window is
  not the wall time you get — actual is `focus_win_ms`, set by the **slowest** node's bit rate
  `[D2]``[D51]`.
- **`results[]` is in MEASUREMENT order** (`.index` = combination id, `.block` stamped), so the
  prefix is always complete: aborts need no compaction, `GET /results.csv?all=1` streams live
  mid-session. ⚠ RAM only — pull it periodically over a long pass; a master reboot loses
  unrepeatable measurements. ⚠ Bare `/results.csv` is the 15-row summary, **not** the record.
- **Blocks are the statistics unit, and ONE BLOCK IS ONE ROUND** `[D76]`. The round boundary is the
  only block boundary: the pass parks there → `/loops` row, drift point, pairwise close, block
  centring, then the camera sweep before the next round scores. ⛔ There is no wall-clock trigger and
  `?calint=` answers **400**; `?cal=0` is the no-sweep control.
  ⚠ **The block length is `?maxruns=`**, so every round holds the same item count and its `Z*`
  values compare — the largest key a block can produce is `(n−1)/√n` in that block's n. The form
  warns past 30 min per round and does nothing else: it is the operator's call.
- Pause stops the clock; Abort publishes the measured prefix.

### Phases
**Phase 0 — scoring.** Each number 1..N gets **exactly one long run** (the session window) in a
fresh Fisher–Yates order. Direction pre-registered: `?score=high|low|abs`, default `high` — it only
picks the pool. **The scoring key is the pass key** — z and concordance at the session's
`?wpre=` `[D48]``[D65]``[D69]`; `score_build_keys()` is the only place a scoring key is built.
⚠ Scoring has no `/loops` block; the scoring span **is** the block. Per-node centre over the
numbers each camera actually answered, then concordance (loudest **centred** node dropped), then
the same mix as the pass. σ is that span's own `[D69]`. Uncentred LSB ranks nodes `[D48]`.
⚠ `?wpre=0` is z alone.
⛔ Never repeat a target in place, in any form `[D5]`.

**Phase 2 — the pass.** One Fisher–Yates order over the whole space, each item measured once.
`PairAcc[i][j]` accumulates per-block-centred moments → full Pearson matrix + per-node σ in
`/status` (flagged if |r|·√n > 3). `close_block()` → `record_loop()` stores per-block offsets/σ and
camera health; `drift_add()` regresses the per-block mean on the block index → `drift_slope`,
`drift_t`; |t| > 3 flags real drift.

⛔ No observer gate `[D66]`. ⛔ No single-pass, no pool-confirmation overlay `[D67]`.
`?unlimited=0` and `POST /pool` answer **400**. The HTML card shows the current number (scoring)
or combination (pass). `confirm=1` writes NVS form prefs only.

### Rounds
**Runs per round** (default 100); `POST /start?maxruns=<n>` (`unlimited` on by default). A **round** =
score every number → keep as many of the best as fit `maxruns` runs → measure that space once in a
fresh Fisher–Yates order → score again. Ends only on **Abort**; the `results[]`-full stop survives
as backstop for a compaction that cannot allocate.

- **Pool sizing maximises COMBINATIONS MEASURED, nothing else**; the bonus preference is only the
  tie-break `[D3]`.
- **Results ACCUMULATE** — `results[]` is never cleared between rounds; every statistic runs on the
  union of all rounds.
- **Every unlimited round compact at the boundary** `[D56]`: the 100 most extreme items by
  `|rank_key|` stay as rows (both tails, so Top-5/Bottom-5 stay exact). The rest merges into
  moments — pass mean/σ/χ² stay exact. A single pass only compact if the space
  would not fit uncompacted `[D42]` — with D67 every session is rounds, so every
  round boundary compact.
  ⚠ The counter and round-base semantics after a compaction (`completed`/`runs_completed`,
  `round_base`/`round_item_base`) are subtle and documented at their definitions in `sensor.h` —
  read them before touching anything that counts or indexes items; getting them wrong is what broke
  the first compaction on hardware `[D42]`.
  ⚠ `compacted=` non-zero in the CSV header: the rows are extremes plus survivors, not a sample —
  never compute a distribution from them.
- **Every round closes its own block, and only the round boundary does** `[D76]`, so centring never
  mixes items from either side of a re-scoring and every block has the same item count. Rounds after
  the first re-run the sweep **before** scoring.
- **No pool-confirmation gate** — the machinery is deleted, not disabled `[D73]`: no
  `PHASE_POOL_CONFIRM`, no `pool_confirm` in `/status`. `pool_auto` stays at 1 as the record.
- ⚠ Inside a round "measured exactly once" holds; **across rounds a combination can recur**. Each
  recurrence is its own row; the identity is **(round, index)**.
- A truncated round draws a **random subset**; only the last round truncates `[D4]`.
- `/status`: `unlimited` (always true), `runs_cap`, `round`, `round_base`, `round_done`, `round_total`,
  `round_start_ms`. `completed` is session-wide, `total` the CURRENT round — ⚠ a progress bar must
  use `round_done/round_total`. `round_start_ms` stamps when MEASURING began (sweep and scoring
  excluded).
- `/status` publishes `pool_main`/`pool_euro` for the whole running session. ⚠ They are withdrawn
  at the ROUND BOUNDARY and before RUNNING, not at the scoring pass — deliberate (stale numbers
  would stand through the insertion and the opening sweep).
- The **Runs per round** field previews pool size and round length from the rate model in
  `sensor.h`: `cycle_ms ≈ 224·segments/rate + CYCLE_FIXED_MS + gap_ms` `[D39]`; the live ETA uses
  measured pace.

---

## The ranking channels
One ranking key from z plus concordance `[D65]``[D68]`:
**key = ((1−p)·z_ctr/σ_z + p·zc_ctr/σ_c) / √((1−p)² + p²)** —
p = `?wpre=` is the concordance weight (API default 0 = z alone, form 0,8). `?score=` only picks the pool.
⚠ A channel that cannot rank an item loses its **weight** too, not just its value `[D75]`: the
normaliser is rebuilt from the channels the item actually has, so an item with z alone is ranked on
z at full scale. Adding a 0 under the two-channel normaliser is a scale error, not a null result.
⚠ Each channel is standardised by ITS OWN σ of **that item's block**, frozen at close `[D68]`. There is **no** session-wide channel σ any more `[D72]` — `s_bsig[block].sig_p`/`sig_c` is the only scale, and `/loops` no longer publishes it. A key with no block σ is 0, never a fallback.
⚠ Uncentred z ranks NODES, not items — a live table is meaningless before the first block closes.
⛔ **The key is UNBOUNDED — nothing truncates an extreme item** `[D75]`. It needs no bound: the item
sits INSIDE the σ it divides by, so `|key| ≤ (n−1)/√n` with n the items in that item's block,
whatever σ comes out. A quiet block cannot manufacture a large key.
⚠ **That ceiling moves with n**, so it is `?maxruns=` that fixes the scale: every round is one block
`[D76]`, so within a session n is constant and Z\* compares. 6,9 for a scoring span's ≤50 numbers,
14,4 at n=208, 23,1 at 535. Pure chance over 38000 items reaches about 4.
⚠ Across SESSIONS with different `?maxruns=` the ceiling differs — that is a pooling question, and
the last round of a session is short because Abort cut it.
UI: **Z\*** is the key itself (block-σ units), **Z**, **Conc**.

**Concordance (D56).** Per node, split the window at nseg/2. Same sign → `√2 · min(|h1|,|h2|)`
with that sign (equals full-window z when the bias is stable). Opposite sign or a zero half → 0.
Then drop the loudest node and Stouffer-combine the rest. k < 2 after the drop → 0.

Wire: `Z:<z>[,<h1>,<h2>][,wsig=<σ>]` `[D65]`. `,wsig=` TAGGED. Every node measures the commanded `nseg`.

## Stored z is RAW; ranking is block-centred
- **`z_score` is the raw combined Stouffer z and is never rewritten.** It is the archive;
  `/results.csv` carries it forever beside the per-node `z0..z3`.
- **`z_ctr` is what every statistic and ranking runs on.** At block close `center_block()` subtracts
  each node's block mean and recombines over the same nodes (`have_mask`, k unchanged). Single
  accessor `rank_z()`. Until a block closes, `z_ctr` holds the provisional raw value. `[D8]`

⚠ **Centring removes any real effect CONSTANT across a whole block.** Pre-registration decision:
what remains visible is an effect varying **between items inside a block**.

### UI
- A **parameter line** for every session, built from `/status`, not the form — a curl-started run
  and a reloaded page both label themselves. "per run" is measured pace once ≥ 5 runs exist, else
  the rate model, and says which. ⚠ The measured value reads high early — `elapsed_ms` also
  contains the opening sweep.
- **Two rows of four stat cards; the split is load-bearing**: top row round-relative in every
  figure, bottom row session-relative in every figure. ⚠ The bottom row is unlimited mode only —
  in a single pass it would repeat the row above it.
- **Two tables of five**: Top-5, Bottom-5, item counter +
  block badge, Save CSV. Columns: `Z*` (key in that item's block-σ units `[D68]`), `Z`, `Conc`,
  `Δn`.
  **`Δn` is node agreement** `[D70]`: σ across the contributing nodes of their block-centred z,
  each node divided by ITS OWN σ over that block. Small = the cameras moved together on this
  item; **≈ 1 is what independent nodes give**, so read it against 1, not against 0.
  ⛔ It ranks and excludes nothing — a confidence figure beside Z*, never a second key `[D70]`.
  ⚠ **—** until the item's block has been centred (same wait as `Z`), and whenever fewer than
  two nodes have a block σ. Never 0 for "unknown": 0 would read as perfect agreement.
  ⚠ Small Δn is agreement, not evidence — centring has already removed the block-wide common
  mode that would otherwise produce it.
- **A third table, the camera-sigma jump board** `[D62]`: the five largest changes in a node's
  own camera noise from its previous measurement to this one, session-wide, named by
  (item/round) plus the drawn numbers. ⛔ **It is a SUSPICION list, not a ranking** — the top row
  is the measurement whose z deserves the LEAST trust. It excludes nothing `[D47]`.
  ⚠ One row per NODE: two nodes on the same item is the light, one node alone is that camera.
  ⚠ Read the **×σ** column, not the raw jump: it scales against `wsig_sd`, the session's own
  measured jump noise (~0,016 on this rig). There is deliberately **no minimum jump** — on a
  quiet session the board holds five rows at 2..3 σ and says so, and the card is shown even
  when empty `[D62]`.
  ⚠ It is filled from the MEASURING pass only; a scoring run has no item to name.
- **A fourth table, Soft-down origins** `[D63]`: when a block trips a node, the three
  measurements of that block whose z sat furthest from the block mean, captured at block close
  because compaction takes the rows one round later. Hidden when nothing tripped.
  Title line: **Block · node · wall time · σ · mean**. Time is `now − (uptime_ms − t_ms)` — no RTC.
  ⚠ A trip belongs to a BLOCK, not to a measurement — σ is the spread over its ~63 items. This
  names what carried the spread, not what "caused" it.
  ⚠ The ×σ column is measured against that block's own spread, not the null: near 3 is
  ordinary. Read the SHAPE — one item far out is an excursion, three close together mean the
  block was simply wide.
  ⚠ The block number shown is 1-based like `/loops`; `results[].block` is 0-based.
- CSV is **German**: `;` separator, `,` decimal — a decimal point makes Excel read text.
- **The start form remembers its last values** (NVS, survives reboot and OTA): measuring time,
  runs per round, direction, concordance weight. `/` serves them as a script chunk
  appended to the page `[D49]`. ⚠ **Only a start carrying `confirm=1` writes them** — i.e. only the
  web UI. A curl start sends no `wpre=` and would otherwise replace the operator's weight with the
  API default. ⚠ It changes **no** API default: an omitted parameter still resolves to the
  compiled-in value. Mode is not remembered — it is which button was pressed, not a field.

### CSV header
`# elotto v3 mode= focus= score= items=<measured>/<planned> ranked= excl= void= blocks= paused_ms=
pass_* v_eff= flush_timeouts= drift_t= unlimited= runs_cap= rounds= run_s= run_segs=
gap_s= compacted= pre_w= pre_n=
fw=<version>/<elf sha>`, then `# nodes=<ip list, discovery order>` and
`# fw_nodes=<sha per node, same order>` (`?` = never answered).
`?all=1` appends a **`round` column last**, then `key;zc_ctr;w0..w3`, so older parsers still line
up on the columns before `round`. `w0..w3` are the per-node CAMERA σ of that item's own window
`[D62]` — empty = not reported; a quiet window reads 1,0, never 0. `zc_ctr` is concordance `[D56]`.
⚠ **`pre_n` counts items the CONCORDANCE term could rank** (`zc_ctr` ≠ 0) `[D73]`. In a
pre-2026-09-02 file the same field counted the second LSB channel and read ≈ `ranked` —
never compare the two across that date.
⚠ The window travels in BOTH units: `run_s` alone cannot separate instrument generations `[D1]`.
⚠ "All four run the same code" is a policy, not a fact — check `fw_nodes`.

---

## ⚠ Pooling — the complete list
Two sessions may be pooled only when every line below holds. Each is a separate instrument or a
separate arm `[D1]`.

| split on | pool only within |
|---|---|
| hardware change 2026-07-29 | after it |
| block centring 2026-08-13 | one side, or recompute both the same way |
| extraction speed-up 2026-08-18 | one side — same `?run=`, 1,85× the bits per item |
| onset flush 2026-08-19 | one side — the bit-to-item mapping changed |
| `focus=on` vs `off` | old attended vs unattended — never mix. Post-D66 is always `off` `[D66]` |
| `pre_w` (2026-08-26) | one weight — for the TABLES. `z_raw`/`z_ctr` pool across weights `[D45]` |
| scoring key 2026-08-28 | one side — a pool chosen before it was chosen on a different key `[D48]` |
| scoring per-node centre 2026-08-31 | post-D69 only — pool chosen like the pass `[D69]` |
| spectral channel deleted 2026-08-28 | post-D53 only — no `ent_w` / z_h `[D53]` |
| runs ranking deleted 2026-08-29 | post-D55 only — no `wruns` / zr `[D55]` |
| concordance / half-window ranking 2026-08-29 | post-D56 only — tables use `zc_ctr` `[D56]` |
| both LSB channels rank 2026-08-29 | post-D58 only — D56 sessions ranked on `zc_ctr` ALONE `[D58]` |
| LSB-as-is 2026-08-31 | post-D65 only — LSB z, no prior archive `[D65]` |
| block-σ ranking 2026-08-31 | post-D68 only — tables in block-σ units `[D68]`. `z_raw`/`z_ctr` still pool |
| unbounded key, per-item weights 2026-09-02 | post-D75 only — earlier keys were truncated at 12 and scaled an item down when it had no concordance. Splits TABLES **and the chosen POOL**; `z_raw`/`z_ctr`/`zc_ctr` still pool `[D75]` |
| v3 vs any v2.x | v3 only |

Unlimited-mode data carries two more: split on `round` before pooling with a single-pass session,
and decide what to do about combinations that recur across rounds before pooling rounds together.

---

## Pass-level health and node health
Read `pass_mean`/`pass_sigma`/`pass_chi2`, `drift_t` and the pairwise matrix before any table.
LSB σ is **not** 1 `[D17]``[D65]` — judge the measured number, do not expect the unit-sigma null.
**The software publishes the numbers and draws no verdict from them**; `PAIR_FLAG_T`/`DRIFT_FLAG_T`
are display hints on `/diag` and gate nothing.

**Soft-down** (`nodes[].soft_down`) takes a node out of the combine after a block with
σ > `NODE_SOFT_TRIP_K` (1,35) × that block's peer-median σ `[D65]`. Sticky; clears after `NODE_SOFT_CLEAR_BLOCKS` (4) blocks under the
peer-referenced bar (`clear_sig`, published per block in `/loops`) `[D12]`. It never reboots
anything. The sweep that follows the block close (the round boundary) is what
recalibrates it — soft-down itself does not call the ladder.
- **σ is the only trip criterion.** |mean| is a flag (`mflag` in `/loops`), never an exclusion —
  centring removes a constant block offset, and the offsets come from the exposure rung `[D11]`.
  ⚠ Replay a threshold change against `/loops` before believing it.
- `NODE_SOFT_MIN_COMBINE` is **1** — a solo combine is possible `[D13]`; `k` is in the CSV per item.
- **Quarantine**: a block a tripping node could have contaminated is excluded from ranking
  (`skip_rank=1`) but stays in the CSV. Fires on every trip, but only when that node was in the
  block's combine `[D14]`.
- A soft-down arm still writes its z to the archive, so any exclusion can be undone offline `[D41]`.

**A node whose camera stalls is REPORTED, DROPPED and REBOOTED** — nothing to fall back to by
design. It replies `E:<reason>`; the master names it in `fault`, drops it, bumps `nodes[].reboots`,
sends `R`; it rejoins the *next* session by discovery. The session aborts (`src_stalled`) only if
the drop would leave fewer than two nodes. ⚠ **The master never reboots itself** — that would
destroy `/loops` and the operator's results. It faults, reports, aborts.

A run that dies part-way produces **no z at all** (`gcp_zscore_raw()` returns false) — archived with
`k=0` (VOID), never ranked, never normalised short.

---

## Current-item display
A "Now:" card shows the number being scored or the combination being measured, in large type.
The session is always unattended `[D66]`. CSV still writes `# focus=off` so new sessions pool
with old unattended, never with `focus=on`.
- The card updates ~70 ms **before** the bits start ⛔ `[D33]`.
- `GET /focus` (~60 B) polled at 10 Hz, separate from the 2,5 KB `/status`; `seq` is monotonic per
  window, so the UI counts *missed* windows — a skipped window names the wrong numbers.
- `POST /pause?on=1|0` holds **between** runs only; state stays `running`, paused time is excluded
  from `elapsed_ms`.
- **Every window starts on fresh bits** (`onset_settle()`, all four nodes flush in parallel on `M`)
  `[D34]`. ⚠ A flush that does not finish **voids the run** (`flush_timeouts`) `[D35]`.
- ⚠ `?focus=` answers **400**.

---

## Camera calibration (per BLOCK)
At every insertion the master broadcasts `K<budget_ms>,<segs>`, sweeps its own ladder in parallel,
and waits for every node's `OK:`. Each node keeps the rung with the **lowest
|bias − 0,5| among candidates that clear the gates**, falling back to the bare gate
if none qualify `[D16]``[D46]`.

⛔ **The key is |bias−0,5| and it is never a gate** `[D46]`. Bias is non-stationary per
node on a timescale of minutes; only the SHAPE across a ladder is stable — it selects, it does not
certify. ⛔ Do not fit an absolute bias bar — one certified-empty a healthy node.
⚠ **The incumbent rung is KEPT** unless a challenger beats it by `CAL_KEEP_MARGIN_K` 3 SE of its
own measurement (`kept` in `/calibrate`). The incumbent is what runs when the sweep STARTS, so a
manual `/expose` gets one sweep of protection — deliberate.

Gates a rung must clear: autocorr < `CAL_AUTOC_TOL` 0,03 (⚠ never subsample — it gates),
σ ≤ `CAL_RAW_SIGMA_K` 1,35 × the ladder's own best (**relative, one-sided**, after the whole
ladder `[D46]``[D65]`), no stuck frames, `mean_px` ≥ 5,0 `[D18]`,
`zero_diff` ≤ `CAL_MAX_ZERO_DIFF` 0,125. ⛔ `CAL_MAX_MEAN_PX` 100 is publish-only `[D52]`.

- **The dark end is gated because photons do the whitening** `[D18]`. Dim the lamp and more rungs
  fail; the answer is light, not a lower floor.
- `raw_runs_z` is published per sweep rung and gates nothing. ⚠ 0,0 in a measurement window means
  NOT ARMED, not "perfectly random" (`raw_trans` says which).
- The budget is a **cap, not a target** (default 10 s, `?cal=<ms>`, 0 = off) `[D21]`. **The trigger
  is the ROUND BOUNDARY** `[D76]` — which also sets the block size and the drift regression's
  resolution, so `?maxruns=` is the knob for both.
- **Nodes land on different exposures on purpose**; what they must share is the segment count.
- `GET /calibrate` serves the whole last sweep per rung with the gate each failed plus the raw
  stats, **on every node** — a per-node optical fault is diagnosable. The chosen setting is
  recorded per block in `/loops`.
- **`/loops` also carries `cam_sig` / `cam_rsig` (stream σ) and `cam_px`** `[D50]`.
  ⚠ **They are cumulative since the last SWEEP, not per block** `[D62]` — `camera_stats_reset()`
  runs only in the sweep and on `/expose`. Up to three blocks share one accumulation here, and a
  block right after a sweep has a shorter one and therefore a noisier σ. For anything that has to
  be located in time use **`GET /camlog` on the node** `[D64]`, or the per-item `w0..w3` in the
  CSV — never these. ⚠ `w0..w3` only survive until the next compaction; `/camlog` is the record
  that does not depend on the master keeping the row.
  ⚠ `cam_px` 0 = the node did not report it, **not** a dark frame.
  ⚠ Every other `cam_*` field is what the last SWEEP found: two blocks on one setting carry an
  identical `cam_bias` and describe neither. That is why a σ 6,94 block was unattributable.
  ⚠ `cam_rsig` is recorded and never gated — no null of 1, non-stationary per node `[D46]`.
- ⚠ `camera_get_stats()` is cumulative since the last sweep; in a `?cal=0` session they are
  lifetime averages.
- ⚠ Never price monitor cost with `/camtest` — the delivered bit rate is the number `[D46]`.
- ⚠ **Never judge `raw_sigma` from single `/expose` readings.** It is non-stationary per node
  on a timescale of minutes: two readings at IDENTICAL settings gave 1,211 and 1,025. Only the
  SWEEP compares — the whole ladder back to back. A gain change was adopted and reverted within
  the hour on exactly this mistake `[D59]`.
- ⚠ **The sweep ladders the EXPOSURE only.** `cal_run()` passes the gain in force at entry
  (`g0`) to every rung, so `CONFIG_ELOTTO_CAM_REG_GAIN` is the operating point for the life of
  the board, not a boot value — and a `POST /expose?gain=` override is carried forward by the
  next sweep instead of being replaced `[D59]`.
  ⛔ **And it stays that way** — gain 1023, exposure ladder only `[D74]`. Laddered on all four
  nodes 2026-09-02: three put their bias optimum at three different gains, slave1 is best at the
  current 1023, and `raw_sigma` — the thing that actually ranks and trips soft-down — does not
  respond to gain at all. It moves the bias, which `center_block()` subtracts anyway.
- ⚠ **Too much light on one node is a fault, not a luxury**: it forces the sweep onto the short
  rungs, which certify worst. slave0 went from 3 of 9 rungs at `raw_sigma` 1,36 to 6 of 9 at
  1,032 after ~6,7× dimming `[D59]`. Judge it by illumination per exposure unit (`mean_px`/`exp`),
  not by `mean_px` alone.

---

## The per-window log — `GET /camlog` `[D64]`
**Every node keeps its own**, 512 windows deep, one entry per measurement window. It is the only
place a disturbance can still be located in time.
- Entry: `t_ms` `tag` `wsig` `wn` `rsig` `rbias` `sig` `bias` `px` `ac1` `zdiff`.
  `tag` = combination id on the master, answered `M` sequence on a slave, **0 = a scoring run**.
- ⚠ **It is a RING** — ~25 min at 2,9 s per window. `dropped` counts what fell out, so a gap
  never reads as a quiet stretch. **Pull it inside the session, per node**; there is no
  master-side collector.
- ⚠ `t_ms` is each node's OWN uptime. Align by `tag` or by ordinal, **never** by subtracting
  timestamps across boards.
- It records what never travels on the wire — `rsig`, `px`, `ac1`, `zdiff`. The wire carries
  `wsig` and nothing else. That difference is what separates "the light moved" from "this sensor
  is dispersing".
- ⚠ Only windows that produced a **Z** are pushed; a faulted or voided run has no window.

## ⚠ A session LOCKS every node's sensor `[D64]`
`/expose`, `/linearity` and `/camtest` answer **409 for the whole session**, on slaves as well as
the master. The slave has no session state of its own and derives one from the master's traffic:
`M`/`K` latch it, `A` releases it, 60 s of silence releases it.
- ⚠ **OTA keeps the NARROW predicate** (`g_measuring`, the ~2 s window) — "abort, then flash" is
  unchanged and `/update` is not blocked for a whole session.
- ⚠ **The idle release is a deliberate hole**: a long parked phase leaves the latch to expire
  under it. It is kept because without it a master crash would leave every slave locked until
  someone rebooted it. (The pool-confirmation park that motivated it is gone `[D66]``[D73]`.)

## Illumination — standing rules
The enclosure is **LIT, not dark** `[D28]`.
- ⛔ **Never power illumination from a node's VSYS pin** `[D29]`.
- ⚠ **After physical work, let the light settle ~30 min** before a long run `[D30]`.
- ⚠ **Do not judge the light by one `mean_px` reading** — sweep, or take a time series.
- ⚠ **The linearity test tells STEADY light from bright light, in a minute**: `GET /linearity`
  on the node in question `[D64]` — it ladders exp 32/64/128/256, reports `mean_px`,
  `px_per_exp` and the ratio per rung, and restores the entry exposure. Steady light doubles
  `mean_px` each time. 409 while a session runs; abort first. On 2026-08-31 slave1
  read 0,86 / 0,65 / 0,39 px per exposure unit — falling, not constant — with `raw_sigma` 6,0 and
  autocorr 0,042. That is FLICKERING light, not too much of it, and no single `mean_px` reading
  can show the difference. After the fix the same node read ×1,94 / ×2,08 / ×2,09 with
  `raw_sigma` 1,03.
- ⛔ **All four nodes on PoE, permanently** (user decision) `[D31]`.
- ⛔ **Do not switch camera hardware** on the theory that the OV5647 is the problem `[D32]`.

---

## Project structure
- **main/elotto.c** – app_main, Ethernet, webserver, HTML/JS UI. Endpoints: `/` `/status` `/start`
  `/abort` `/loops` `/results.csv` `/focus` `/pause` `/calibrate` `/pool` (400) `/probe`
  `/expose` `/diag` `/diagjson` `/camtest` `/camlog` `/linearity`, +5 from elotto_ota.
  ⚠ The URI-handler cap fails silently (404, return value unchecked) — the count lives at
  `start_webserver()`; prefer `?all=1` on an existing endpoint over a new handler.
  **`GET /diagjson?all=1` is the COLLECTOR**: the whole array's front-end health in one request,
  discovery order, IP per row `[D43]`. 409 while measuring. ⚠ `cal_*` is what the last SWEEP chose;
  `exposure`/`gain` are LIVE — they differ after a manual `/expose` or an uncertified sweep.
  `POST /expose?exp=<lines>[&gain=<g>]` sets one node's live operating point (tuning the physical
  LIGHT against a live reading; resets camera stats so `mean_px` answers in ~2 s). Not sticky —
  the next sweep overwrites it, correctly. 409 while measuring.
- **main/sensor.c/h** – GCP analysis, scoring/pooling, the pass, blocks, centring, drift,
  soft-down, publishing. ⚠ Counter and round-base semantics are commented at the definitions in
  `sensor.h`.
- **main/nodes.c/h** – the array: UDP link, discovery, calibration handshake, per-node health,
  drop/reboot policy. sensor.c reaches other boards only through `nodes.h`.
- **main/focus.c** – current-item card, pause, run gap, session clock — one file because they share state.
- **ota_firmware/** – the network updater, its own IDF project. Ethernet + HTTP + esp_ota only.
- **components/elotto_camera/** – OV5647 entropy extraction. One stream `[D65]`; the runs half
  is armed only inside a sweep `[D46]`. Also serves `/camlog`, `/linearity`, `/expose`,
  `/calibrate` and `/camtest` for **every** node from one implementation — the four boards must
  not describe their own cameras in four different shapes.
- **components/elotto_link/** – the UDP wire format (`EL1 <seq> <payload>`, ports 5000/5001), plus
  `EL_SEG_MIN`/`EL_SEG_MAX` as ONE definition for both firmwares.
- **components/elotto_gcp/** – the z primitive (`gcp_zscore_raw()`, `gcp_z_per_bias()`), shared so
  the nodes cannot disagree about what a z is `[D37]`. ⚠ The remaining soft-float calls per segment are deliberate `[D26]`.
- **components/elotto_ota/** – update endpoint + boot safety (`/update` `/boot` `/reboot` `/poison`
  `/otainfo`; `BOOT_ATTEMPT_LIMIT` 3, `HEALTHY_UPTIME_MS` 30000). ⚠ `fw_boot_attempts` counts
  boots since the last run that stayed up 30 s, **not** failures — every node reads 1 for its
  first 30 s after a flash.

All three components are **shared**: the slave repo pulls them via
`EXTRA_COMPONENT_DIRS=../elotto/components`; ota_firmware points at the single elotto_ota directory
(IDF compiles every component it discovers — `components/` would drag the camera into the recovery
image). ⚠ The repos must stay siblings on disk; build, flash and commit them together.

### Wire protocol
`P` discovery · `M<seg>` measure · `K<budget_ms>,<segs>` calibrate · `D` diagnostics · `A` abort ·
`R` reboot. Replies `OK`, `Z:<z>[,<h1>,<h2>][,wsig=]`, `D:`, `E:<reason>`, `V:<reason>` (void, not a
fault). `h1`/`h2` are z of the two halves of the same window `[D56]`; a node that sends none
is ranked on the full window (leave-one-out still applies). Extra fields ignored.
UDP loss is handled explicitly: every frame carries the sequence it answers, mismatches are dropped
and counted (`net_stale`), a timed-out command is resent under the same sequence so a node replies
from a one-entry cache instead of measuring twice.
⚠ All receive timeouts go through `link_arm_timeout()` — its comment in `nodes.c` says why the
floor is load-bearing.
⚠ A receiver does not clamp an out-of-range segment count, it substitutes its own `[D38]`;
`LINK_MEAS_MS_FOR(nseg)` scales with the run and is deliberately generous `[D7]`.

### Resources
- **PSRAM is mandatory**: capture buffers, extraction ring, `loop_hist`, per-item per-node archives
  (`s_node_z` ~128 KB). Internal RAM is full with `results[]` — a few KB more of .bss fails the
  *link*, not the run.
- ⚠ **Task priority is load-bearing**: any task calling `camera_read_word()` must run **above** the
  extraction task, or the consumer starves ~10× (see `camera.h`; signature: ring `drops` huge with
  `waits == 0`).
- ⚠ **Task CORE is load-bearing too, and it is a separate rule.** Every task in the measuring path
  is created with `xTaskCreatePinnedToCore()`: extraction on `ELOTTO_CAM_TASK_CORE`, consumer and
  web server on `ELOTTO_CAM_CONSUMER_CORE` (both in `camera.h`) `[D61]`. Plain `xTaskCreate()` is
  `tskNO_AFFINITY`, and the master creates `elotto_task` fresh on **every** `/start` — unpinned it
  shared a core with `cam_task` in about one session in three and **everything halved**:
  `focus_win_ms` 10,2 s instead of 5,13, consumption 2,88 instead of 5,77.
  ⚠ The signature is consumption halving EXACTLY while all other nodes stay normal.
  ⚠ **One fast session proves nothing here** — it was intermittent. Re-test any new task in this
  path over ~20 session starts.

---

## Nodes
| node | IP | MAC | COM | flash contents |
|------|----|-----|-----|----------------|
| master | 192.168.178.100 | 80:f1:b2:d2:e3:1d | COM4 | factory = updater, ota_0/ota_1 = elotto app |
| slave0 | 192.168.178.103 (static lease) | 80:f1:b2:d2:e3:e5 | — | factory = updater, ota_0 = slave app |
| slave1 | 192.168.178.145 (static lease) | e8:f6:0a:e0:ce:a8 | — | factory = updater, ota_0/ota_1 = slave app |
| slave2 | 192.168.178.155 | e8:f6:0a:e0:c7:a1 | — | factory = updater, ota_0 = slave app |

**Say master/slave0/slave1/slave2, never the IP ending.**
⚠ **Column order in a results CSV is DISCOVERY order**, not the slave number, and changes between
sessions — the IP list in the header is what makes `z0..z3` decodable; never map by position.
⚠ **COM ports are not stable** — list the ports before an `erase-flash`; a wrong port wipes a
working node.
Addresses are informational: the master finds slaves by UDP broadcast.

### Extraction — where the rate stands
Idle production is **~7,4 Mbit/s** post-D65 (adjacent-pixel XOR off, 2× words). The LOADED rate has
not been re-measured since — the pre-D65 figures (5,71 idle, ~3,7 loaded) are a different
instrument `[D22]``[D25]`.
- ⛔ Nothing done to the extraction path can raise the **idle** rate `[D23]`; the loaded rate is
  still open — prove any change with `ms_extract` under load, never at idle `[D25]`.
- `/diagjson` publishes the per-pair split on every node: `ms_pair` = `ms_wait` + `ms_extract` +
  `ms_rest`.
- ⚠ **`mbit_s` is PRODUCTION, `consume_mbit_s` is what a measurement READ** `[D60]`. The first
  counts words the ring then discarded — it read 5,71 on the master against 3,34 on the slaves
  purely because the master's consumer was slower and its ring overflowed. The node table and
  `/diag` show the second; use the first only for the sensor ceiling `[D23]` and `/camtest`.
- ⚠ **If `focus_win_ms` ever doubles** (~5,13 s → ~10,2 s at identical parameters), suspect an
  unpinned task in the measuring path, not the camera `[D61]` — signature: consumption halves
  exactly on that node alone. The rule is under **Resources**; this is how it surfaces here.
- **`GET /camtest`** runs the byte-wise reference against the word-wise path on the node's own
  silicon; `cam_extract_ref()` stays compiled in as the definition of correct.
  ⛔ **Do not change the live extractor without it.** 409 while measuring; master only.
  ⚠ Its `ns_*` vary ~10 % (it benchmarks while the capture task extracts) — for live cost read
  `ms_extract` from `/diagjson`.

---

## Build, Flash, Monitor
**Build** — always through `build.ps1`, which sets the environment and forwards its arguments.
Shell state does not survive between tool calls, and the script must use the **VS Code
extension's** venv (`C:\Espressif\tools\python\v6.0.1\venv`), not `export.ps1`'s, or the build dir
gets pinned to the wrong interpreter and fails with "run 'idf.py fullclean'".

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
`POST /boot?slot=N` selects the next boot slot and clears the fail counter — that is how a node is
put back on a known-good image without USB.

- `/update` and `POST /start` return **409** while a session runs. Abort first.
- ⚠ **Never OTA-flash while a session is running without asking the operator first.**
  Check `/status` `state` — if `running` (or not clearly idle/done/aborted), **ask** before
  aborting or flashing. Do not silently abort a live measurement to push firmware.
- ⚠ **After every OTA, poll `fw_sha` in `/status` until it CHANGES** `[D27]`.
- ⚠ **A node that pings but refuses port 80 is not dead** — check `/otainfo` before USB `[D40]`.
- **USB is only for** a fresh board or a node whose recovery updater is gone:
  `.\build.ps1 -C ota_firmware -p COMx erase-flash`, then `... -p COMx flash`.

## Rules
- ⚠ **Never edit sdkconfig manually.** Edit `sdkconfig.defaults`, delete `sdkconfig`, let the build
  regenerate it, verify the diff. Every project has a `sdkconfig.defaults` and sets `IDF_TARGET` in
  its CMakeLists — without both, a regenerate loses settings or fails.
- Target is always esp32p4.
- ⚠ The slave's `sdkconfig.defaults` must keep `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` **before**
  `ESP32P4_REV_MIN_0=y` — without it the build silently targets rev v3.1 and refuses to boot on
  these v1.3 boards.
- Firmware is delivered **over OTA only** in normal workflow.

---

## Where things stand
Status snapshot (history, not rules): **[docs/STATUS.md](docs/STATUS.md)**.
Closed §1.x design notes: **[docs/PLAN_HISTORY.md](docs/PLAN_HISTORY.md)** (stub → `git show 1e62bca:…`).
Former v3 PLAN: **[docs/PLAN.md](docs/PLAN.md)** (stub → `git show 4c58802:…`).
⚠ STATUS / PLAN / PLAN_HISTORY are history, not rules. Everything above this line is the rule set.
