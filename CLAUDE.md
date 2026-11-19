# elotto – ESP32-P4 Project

**Rules live here.** Historical decisions and their justifications were deliberately removed:
this is an active research project — do not restore the old evidence log or re-import history from
git/GitHub, and do not treat superseded history as current rules. Work from the current code and
these rules; re-measure what you need.
**Keep docs terse and current.** Write only what something **is** and how to read it — no dated
“why”, no measured anecdotes, no change history. When a rule or number is superseded, **delete** it,
don’t annotate it. Git is the only archive.
**Module-local traps live as comments at the code site** —
this file keeps only what bites across files or from the operator's side.

Two markers, and only two:
**⚠** a trap that bites the next time you touch this ·
**⛔** decided, do not re-open.

## Purpose
Rank lottery **numbers** (scoring → pool) and **combinations** (the pass) from camera readout noise.
One key, the same function for both — Z\*, defined under **The ranking channels**. Scoring
**selects** the pool on that key; the pass **ranks** items on the same key.

**How the cameras stay usable.** One OV5647 **or** IMX219 per node — `esp_video` probes both (OV at I2C 0x36, IMX at 0x10) and binds whichever chip answers. Frame-pair LSB — z is the camera bits. IMX219 streams packed RAW10 (4 pixels in 5 bytes); the extractor takes the 10-bit LSB. An OV5647 streams RAW10 1920×1080 unbinned on the slaves, RAW8 800×800 on the master (`sdkconfig.defaults` per repo). ⚠ **Never write an OV5647's registers while the sensor streams** — it stalls the MIPI output without tripping `stalls`. Its on-chip processing (ISP bits `0x5000`, black level `0x4009`, NVS overrides) and the buffer-layout trap: `components/elotto_camera/CLAUDE.md`.
⚠ An IMX219 measures no `px` and no `zero_diff` (both read 0) and nothing gates on light —
the enclosures are dark by decision; only `/linearity` measures `px` (above black, pedestal 16
subtracted). `/status` `cam_sensor` names the bound chip. The **sweep**
picks the exposure with the lowest **`raw_sigma`** among rungs that still look like noise (autocorr,
dark / zero_diff)  — dispersion is what `rank_key()` divides by and what soft-down trips on;
the bias is subtracted by the centring and costs nothing. If a node's block σ is too loud against its peers, **soft-down**
takes it out of the combine; the next sweep recalibrates every node including the tripped one, and
it returns after four clean blocks. When sweeps run: **Camera calibration**.

## Concept
Ten-node ESP32-P4 array (`MAX_NODES` 10; per-node masks are `node_mask_t` = `uint16_t`, so 16 is the type ceiling). The master scores lottery numbers via GCP methodology; up to nine slaves
measure the same window in parallel, triggered by one UDP broadcast (port 5000). Slaves are
discovered by broadcast at every session start — no IP table, no node count configured. Combined
z = **Σ z_node / √k** over the k nodes that answered *that* run, so a missing reply costs that
run's gain, not the session.

**Entropy source** (user decision): **every node runs in total DARKNESS** and its entropy is the
sensor's own readout noise — pixel transistor thermal and trap noise, column/ADC chain — at maximum
analog gain. ⚠ A dark OV5647 needs RAW10: on RAW8 its readout noise spans too few steps and the LSB
is biased and over-dispersed. One camera per node, never shared.
Non-overlapping frame pairs, diff = f[2k+1]−f[2k] per pixel (cancels FPN exactly), LSB packed.
⛔ LSB bits as measured, the dark IMX219 and the OV5647 included — the higher diff bits are biased or sign-dominated (`GET /camtest?bitscan=<s>`, docs/BITSCAN.md). Fisher–Yates uses an xorshift32 seeded from the camera; it never enters a z.

**The ×√n gain is NOT established** — it assumes node independence. Judge a session on per-block
combined σ **and** the full pairwise matrix, never on `pair_r` alone: **σ, not correlation, is
where this array fails** (measured: σ 1,378 with every pairwise |r| ≤ 0,024).

Modes: Eurojackpot (5 of 50 + 2 of 12, 7920 combinations) and 6 of 49 (5005).
⚠ `?mode=` is `1` for 6-of-49 and **anything else** for Eurojackpot — it tests `val[0]=='1'`, so
`?mode=649` silently starts a Eurojackpot session.

---

## v3 — rounds until Abort or 72 h
**This file is the contract, as rounds.** (`docs/PLAN.md` is a stub.) A round scores every number, keeps as many of
the best as fit `?maxruns=` (default 100), measures that space once in a Fisher–Yates order,
then scores again. Ends on **Abort**, after `SESSION_MAX_MS` 72 h of session time (pauses
excluded), or when the full table is full — the last two end as `done` with the reason in
`/status` `fault` and in the Log, the open block centred like an abort. Inside a round each combination is measured exactly once;
across rounds a combination can recur — identity is **(round, index)**.

- **No loops, no Runs cap, no ranking modes.** `/start` is a **whitelist** (unknown key → **400**,
  out-of-range value → **400**, no fallback), and ⚠ a refused /start changes nothing. Every key, range
  and default — `?run=`/`?srun=` 0,2–22 s, `?gap=`, `?maxruns=` 0 or 10..400 (`?maxruns=0` =
  scoring-only), `?passes=0` (continuous, only with `?maxruns=0`), `?cal=`: skill `start-params`.
  `NUM_RUNS` 1400 caps `results[]`; a round is sized to `?maxruns=` and never truncated.
- **`results[]` is in MEASUREMENT order** (`.index` = combination id, `.block` stamped), so the
  prefix is always complete: aborts need no compaction. ⚠ RAM only — a master reboot or the
  next session start ends it; Save (see UI) writes it out while it is still in RAM.
- **Blocks are the statistics unit, and ONE BLOCK IS ONE ROUND**. The round boundary is the
  only block boundary: the pass parks there → `/loops` row, drift point, pairwise close, block
  centring, then a sweep candidate before the next round scores (when a sweep runs: **Camera
  calibration**). `?calint=` answers **400**.
  ⚠ **A sweep is not a block boundary.** Sweeps happen only at candidate points, never inside the
  pass block, so the 2 h can be overrun by at most one pass. Every candidate sits on a centring
  boundary: every scoring PASS is its own centring span (`score_build_keys()` runs per pass), and
  the pass block is its own. No operating point ever moves underneath a mean being subtracted.
  ⚠ **The block length is `?maxruns=`**, so every round holds the same item count and Z/σ_z
  compares. That part is at most `(n−1)/√n` in the block's n; the factors are not.
  (⚠ With `?maxruns=0` there is no pass and therefore no block at all.)
- Pause stops the clock; Abort publishes the measured prefix.

### Phases
**Phase 0 — scoring.** It starts 2 s after the sweep before it (`SCORE_START_PAUSE_MS`, a visible
break for the operator). Each number 1..N is measured **`?passes=` times** (1..`SCORE_PASSES_MAX` 100, default 20;
0 = continuous, the `?maxruns=0` rule under v3), each pass one
`?srun=` window per number in a fresh Fisher–Yates order (never the same number back-to-back).
After each pass every column — Z\*, Z, Disp, Δn, Ser, Mau, Byt — is added to that number's own running sum;
the pool is the top by ONE of those sums, the one selected on the page (Σ marker in a column
header, `POST /scoresum?c=key|z|disp|nsd|ser|mau|byt`) **when the whole scoring
ends** — in Eurojackpot both pools at once. ⚠ The pick compares the sum **per pass that added to
it** — Σ/√n, for Δn Σ/n (`score_rank_val()`) — so a number that lacks a pass is neither favoured nor
penalised; with the same n everywhere that is the order of the sums. The cell, the scoring table's
sort and Save keep the bare sum. The pool criterion is **freely
selectable and remembered**: a Σ click writes it to NVS at once (any start, not only
`confirm=1`), boot loads it (Z\* if none stored), a session start keeps it — it holds for the whole
session and the following ones until the next click. The start form shows it read-only
("Pool criterion"). `/status` `score_sum` names it. Direction: `?score=high|low`, API default `high` — it only
picks the pool. Remembered like the column: the Σ arrow writes it to NVS at once
(`POST /scoresum?dir=`), and during the scoring it also applies to the running session.
Σ Z\* is ≥ 0: `high` is the largest, `low` the smallest.
Every other column stays signed (`high` = largest, `low` = smallest). The same rule orders the
scoring table and the item table on Σ Z\* and on the Z\* column. The scoring table
(`GET /extremes?score=1`, `RunResult` rows in their own buffer, never in `results[]`) is display
only — the pool is picked from the sum in `elotto_task()`. Until a pass has closed a
number's Σ is a dash and the rows stand in number order; the table runs at least 10 rows and down
to the last pool member. **The scoring key is the pass key** — `zstar_of()`, after that pass's Mau exists.
⚠ Scoring has no `/loops` block; the scoring span **is** the block. Per-node centre over the
numbers each camera actually answered, then
the same product as the pass. σ is that span's own. Uncentred LSB ranks nodes.
⚠ All four weights at 0 is z alone (Z/σ). `score_build_keys()` centres the span and publishes its σ; it does not build the key.
The seven sums of every number (main and euro each their own record) are kept for every scoring
of the session until the next session start: `SCORE_SNAP_MAX` 128 earliest, PSRAM,
`GET /extremes?sums=1`. A scoring aborted before any pass closed stores nothing; one aborted
after closed passes stores those passes. Abort does not clear it. Save writes it.
⛔ Never repeat a target in place, in any form.

**Phase 2 — the pass.** One Fisher–Yates order over the whole space, each item measured once.
`PairAcc[i][j]` accumulates per-block-centred moments → full Pearson matrix + per-node σ in
`/status` (flagged if |r|·√n > 3). `close_block()` → `record_loop()` stores per-block offsets/σ and
camera health; `drift_add()` regresses the per-block mean on the block index → `drift_slope`,
`drift_t`; |t| > 3 flags real drift.

⛔ No observer gate. ⛔ No single-pass, no pool-confirmation gate — the machinery is deleted, not
disabled. `?unlimited=0` and `POST /pool` answer **400**.
`confirm=1` writes NVS form prefs only.

### Rounds
**Runs per round** `?maxruns=<n>` (default 100) sizes the pool of each round. The `results[]`-full
stop survives as backstop for a compaction that cannot allocate.

- **Pool sizing maximises COMBINATIONS MEASURED, nothing else**; the bonus preference is only the
  tie-break.
- **Fixed numbers** `?fix=x` (form "Fixed numbers", 0 = off, remembered): the x best-scored main
  numbers on the pool criterion are in **every** combination of the pass; the other nm−x come from
  the next-best numbers, sized like any pool (main space C(p−x, nm−x) · C(q,2) ≤ `?maxruns=`, the
  free part may grow to every remaining number). 6 of 49: x = 3..6, Eurojackpot: x = 2..5; the
  bonus pool is sized as always. Needs `?maxruns=` ≥ 10, else **400**; a value outside the mode's
  range **400**. `/status` `pool_fix`; `pool_main` lists the fixed numbers first.
  ⚠ x = nm is ONE main combination: 6 of 49 x = 6 measures a single item per round, so its block
  has no σ and every Z\* of it is 0.
- **Results ACCUMULATE** — `results[]` is never cleared between rounds; every statistic runs on the
  union of all rounds.
- **Every round compact at the boundary**: the 1000 items that rank best on the **pool
  criterion** stay as rows — the Σ column (`score_sum`: Z\*, Z, Disp, Δn, Ser, Mau, Byt) in the pool
  direction (`score_dir`: high = largest values, low = smallest; `rank_order()` in `sensor.c`).
  Both can only change during a scoring, so a round's compaction uses exactly the criterion its
  pool was picked with. ⚠ **One tail only.** On Z\* the comparison is |Z\*| (the rule under
  Phase 0); every other column stays signed, so under `high` on Z the most negative values leave.
  ⚠ A row without a value in that column (Δn
  with k < 2, Z* of a block without σ) is never kept. ⚠ A criterion switched in a later scoring cannot bring back rows an
  earlier compaction merged. Quarantined rows are not kept as rows — they go to the full table,
  flagged. The rest merges into moments — pass mean/σ/χ²
  stay exact. `n ≤ 1000` is a no-op, so a short session keeps every row. The Top-10
  (`GET /extremes`) chooses from this archive plus the open round while the session runs.
- **The full table**: every ranked or quarantined row a compaction merges is first copied to PSRAM with its
  Z\* (`FULL_MAX` 250000 rows, ~13 MB; no per-camera values — Δn carries the agreement). Once
  the session is over, `/extremes` (Top-10, any column and direction, and Save's `?n=1000`)
  chooses from the full table plus the rows still in `results[]` — every ranked pass item of the
  session; `held` is that count. ⚠ Only after the session: one request scans the whole table on
  the HTTP task. ⚠ Scoring runs are not in it. When fewer than `UNLIM_RUNS_MAX` rows are left the
  session ends, so the table is never incomplete. RAM only, like `results[]`. The selection of a
  finished session is kept, so the page's repeat polls cost no scan. It is allocated at the
  session start in ten pieces; pieces that cannot be had shorten the table (the session then
  ends earlier) and the Log says by how much, none at all leaves the held rows as the set.
  Save writes the whole table (`elotto_all<n>_….csv`).
  ⚠ Block σ is kept for `BSIG_MAX` 8192 blocks (`sensor.c`), not `LOOP_HIST` 1024 — a row past
  it would have no Z\*.
  ⚠ The counter and round-base semantics after a compaction (`completed`/`runs_completed`,
  `round_base`/`round_item_base`) are subtle and documented at their definitions in `sensor.h` —
  read them before touching anything that counts or indexes items.
  ⚠ `compacted` non-zero in `/status`: `results[]` holds extremes plus survivors, not a sample —
  never compute a distribution from its rows.
- A truncated round draws a **random subset**; only the last round truncates.
- `/status`: `unlimited` (always true), `runs_cap`, `round`, `round_base`, `round_done`, `round_total`,
  `round_start_ms`. `completed` is session-wide, `total` the CURRENT round — ⚠ a progress bar must
  use `round_done/round_total`. `round_start_ms` stamps when MEASURING began (sweep and scoring
  excluded).
- `/status` publishes `pool_main`/`pool_euro` for the whole running session. ⚠ They are withdrawn
  at the ROUND BOUNDARY and before RUNNING, not at the scoring pass — deliberate (stale numbers
  would stand through the insertion and the opening sweep).
- The **Runs per round** field previews pool size and round length from the rate model in
  `sensor.h` (the page takes `CYCLE_FIXED_MS` and the rate pair from it): skill `ui`.

---

## The ranking channels
One ranking key, `zstar_of()` in `sensor.c`, used by the scoring pass and by `rank_key()`:

Z\* = √[ (γ·Z/σ_z)² + (ζ·Disp)² + (α·max(0, z_Δ))² + (ε·Ser)² + (β·Mau)² + (η·Byt)² ]

γ α β ε ζ η are `?kz=` `?kdn=` `?kmau=` `?kser=` `?kdisp=` `?kbyt=`, each 0..1 and independent; 0 leaves that channel out; default γ = 1, the rest 0, so Z\* = |Z/σ_z|. Every channel is in σ of its own block, so a 3 weighs the same anywhere: Mau 4 alone is 4, Z/σ_z 0,3 with Mau 4 is √(0,09 + 16) ≈ 4,0. **Z\* ≥ 0**; each channel's sign stays in its own column. A missing channel adds nothing. z_Δ = Φ⁻¹(1 − P_χ²(df)(df·Δn²)) (`dn_z()`), clamped to ±5; df = Σ over the five channels of (nodes − 1), 45 at 10 nodes: Δn = 0,8 → +1,9. Only agreement counts.
**Σ Z\* in the scoring is not a sum of Z\***: each channel is summed with its sign (Z as Z/σ_z of its pass, Δn as z_Δ), and Z\*_Σ = the formula above on S_c/√n_c; the stored sum is √n·Z\*_Σ, so the Σ/√n pick ranks on Z\*_Σ. Z/σ_z +2 and −2 over two passes give 0. ⚠ The start form types each weight as a whole percent 0–100 and sends the fraction: 80 in the field is `?kdn=0.80`. `?kdn=80` is refused. A column enters Z\* only through its weight; as the pool criterion it still picks the pool and the extreme set on its own sum.
- **Z** — binomial of the camera LSBs in that window, block-centred Stouffer combine; σ_z is that block's own σ (scoring: the pass span).
- **Mau, Disp, Ser, Byt** — per node a window statistic (Maurer z, window σ, lag-1..4 serial z, byte χ² z), centred on that node's block mean (scoring: the pass mean; a node with < 2 values stays out), Stouffer over the combined nodes, divided by the block's own σ of that combine: unit variance, read against 0, **—** until centred.
- **Δn** — node agreement **pooled over Z, Mau, Ser, Disp and Byt** (`node_span_agree()`): per channel each node centred and divided by its own σ over the span (÷ n, not n − 1: in-span scaling would otherwise read Δn ≈ √((n−1)/n), 0,93 at n = 7), squared deviations from the nodes' mean summed over the channels, ÷ df. Read against **1**, null σ ≈ 0,1 at 10 nodes; enters Z\* as z_Δ, only below the null; **—** (never 0) when unknown. ⚠ A pass row stores no df: it is taken as 5·(k − 1), exact when every combined node reported every channel and has ≥ 3 items in the block. Scoring uses the exact df.
- **Sync** — published only; enters no Z\*, no sum, no ranking.
- How each column is built, its thresholds, the worked stack and the wire tags: skill `ranking-channels`.

⚠ Ser and Byt are stored per row in thousandths (±32; `run_ser()` / `run_byt()`), so the `results[]` row stays 48 bytes.

⚠ Z is divided by the σ of **that item's block**, frozen at close. There is **no** session-wide σ — `s_bsig[block].sig_p` is the only scale, and `/loops` no longer publishes it. A key with no block σ is 0, never a fallback. Z\* is not re-standardised: with m channels on, Z\*² is ≈ χ² with m degrees of freedom under independence, so a row with a missing channel ranks lower.
⚠ Uncentred z ranks NODES, not items — a live table is meaningless before the first block closes.
⛔ **The key is UNBOUNDED — nothing truncates an extreme item**. Z/σ_z sits inside its block (`|Z/σ_z| ≤ (n−1)/√n`), z_Δ stops at 5; Ser, Disp, Mau and Byt do not, and a quiet block can then produce a large key. ⚠ The last round of a session is short because Abort cut it.

## Stored z is RAW; ranking is block-centred
- **`z_score` is the raw combined Stouffer z and is never rewritten.** It stays beside `z_ctr` in
  `results[]` for the session.
- **`z_ctr` is what every statistic and ranking runs on.** At block close `center_block()` subtracts
  each node's block mean, divides each node by ITS OWN σ over that block and recombines
  (`have_mask`, k unchanged) — every node carries the same weight and the combine has unit variance
  whichever nodes answered; (z − mean)/σ of 1,0 1,0 1,0 −2,0 over four nodes is +0,50. ⚠ A node
  with fewer than three values in that block (scoring: that pass) has no σ and stays out of the
  recombine; with no σ at all the combine is centred only, with no mean either it is the raw one. Single
  accessor `rank_z()`. An open block is
  centred provisionally from `PASS_OPEN_MIN_N` 4 items on; below that `z_ctr` is the raw value.

⚠ **Centring removes any real effect CONSTANT across a whole block.** Pre-registration decision:
what remains visible is an effect varying **between items inside a block**.

### UI
Page details (parameter line, stat cards, Top-10, Save files, jump board, soft-down origins,
start-form memory): skill `ui`.
- ⚠ `/extremes` ranks every held row on the master's HTTP task, which shares the consumer core:
  poll it no faster than every **5 s**, a header click at most once per second.
- ⛔ **The camera-sigma jump board is a SUSPICION list, not a ranking** — the top row is the
  measurement whose z deserves the LEAST trust. It excludes nothing.
- ⚠ **The start form's memory changes no API default**: only a start with `confirm=1` writes it,
  and an omitted parameter still resolves to the compiled-in value.

---

## ⛔ A session is a closed unit
Everything is judged **inside one session**: its blocks, its pairwise matrix, its tables. Sessions
are never compared, pooled or merged. Save writes the files of the session still in RAM; a reboot
or the next start drops them. Parameters, firmware and instrument may change freely between
sessions; what must hold is that they do not change **within** one.

---

## Pass-level health and node health
Read `pass_mean`/`pass_sigma`/`pass_chi2`, `drift_t` and the pairwise matrix before any table.
LSB σ is **not** 1  — judge the measured number, do not expect the unit-sigma null.
**The software publishes the numbers and draws no verdict from them**; `PAIR_FLAG_T`/`DRIFT_FLAG_T`
are display hints on `/diag` and gate nothing.

**Soft-down** (`nodes[].soft_down`) takes a node out of the combine after a block with
σ > `NODE_SOFT_TRIP_K` (1,5) × the median of all nodes' σ in that block (its own included).
⚠ A node is judged only with `NODE_SOFT_MIN_N` 20 items in the block and at least two nodes — a
round of `?maxruns=` 10..19 can never trip, so a loud camera stays in the combine there. Sticky; clears after `NODE_SOFT_CLEAR_BLOCKS` (4) blocks under the
peer-referenced bar (`clear_sig`, published per block in `/loops`). It never reboots
anything. The sweeps that follow the block close (the round boundary, then the one before that
round's pass) are what recalibrate it — soft-down itself does not call the ladder.
- **σ is the only trip criterion.** |mean| is a flag (`mflag` in `/loops`), never an exclusion —
  centring removes a constant block offset, and the offsets come from the exposure rung.
  ⚠ Replay a threshold change against `/loops` before believing it.
- `NODE_SOFT_MIN_COMBINE` is **1** — a solo combine is possible; `k` is kept per item.
- **Quarantine**: a block a tripping node could have contaminated is excluded from ranking
  (`skip_rank=1`) but stays in `results[]`. Fires on every trip, but only when that node was in the
  block's combine. Its rows stay out of every table and statistic; compaction copies them to the
  full table, and Save's `elotto_all<n>_…` writes them with `Quarantined` = 1 (`excl` in
  `/extremes?full=1`).
- A soft-down arm still records its per-node z (`s_node_z`).

**A node whose camera stalls is REPORTED, DROPPED and REBOOTED** — nothing to fall back to by
design. It replies `E:<reason>`; the master names it in `fault`, drops it, bumps `nodes[].reboots`,
sends `R`; it rejoins the *next* session by discovery. The session aborts (`src_stalled`) only if
the drop would leave fewer than two nodes. ⚠ **The master never reboots itself** — that would
destroy `/loops` and the operator's results. It faults, reports, aborts.

A run that dies part-way produces **no z at all** (`gcp_zscore_raw()` returns false) — archived with
`k=0` (VOID), never ranked, never normalised short.

---

## Windows
The "Now:" card, `/focus` and `/pause`: skill `ui`. The session is always unattended.
- **Every window starts on fresh bits** (`onset_settle()`, all nodes flush in parallel on `M`;
  the first pair after `M` is discarded unextracted)
. ⚠ A flush that does not finish **voids the run** (`flush_timeouts`).

---

## Camera calibration (per BLOCK)
At every insertion the master broadcasts `K<budget_ms>,<segs>` and every node sweeps its exposure
ladder, keeping the rung with the **lowest `raw_sigma` among candidates that clear the gates**; the
incumbent stays unless a challenger beats it by 3 SE. Keep margin, `/calibrate` and the `/loops`
`cam_*` fields in detail: skill `calibration`.
**An IMX219 does not ladder**: exposure fixed at 1600 lines, the sweep is a health check that never
moves the rung — so on this all-IMX219 array the settle pause below never fires — with no light
gate (no DARK/ZDIFF). Details: `components/elotto_camera/CLAUDE.md` — read it before interpreting
`/calibrate`.
Sweeps run at session start and then at candidate points (end of a scoring pass, before the pass,
round boundary) on an interval of 15 min … 2 h; the schedule, the budget and the readout traps
(`cam_*`, `raw_sigma`, `/camtest`): skill `calibration`.

⛔ **The bias gates nothing and selects nothing**. It is non-stationary per node on a
timescale of minutes, only the SHAPE across a ladder is stable, and the centring subtracts it
anyway. It stays measured and published. ⛔ Do not fit an absolute bias bar — one certified-empty a
healthy node.
⛔ **The sweep ladders the EXPOSURE only**; the gain stays at the sensor's maximum analog gain for
the life of the board — `CONFIG_ELOTTO_CAM_REG_GAIN` 1023, clamped to the chip's ceiling
(`cam_gain_max()`: IMX219 232, OV5647 1023). An IMX219 reading 232 is this rule, not a violation.
- ⚠ **Any sweep that moves any node's exposure is followed by a 60 s settle pause of the whole
  array** (`CAL_SETTLE_AFTER_MS`), nothing measured — a fresh rung trips soft-down on its own.

---

## The per-window log — `GET /camlog` 
**Every node keeps its own** ring of 512 windows (~25 min) — the only place a disturbance can
still be located in time. ⚠ Pull it inside the session, per node; align by `tag`, never by
timestamps. Fields, ring and `rec` record: skill `camlog` — read it before interpreting a reply.

## ⚠ A session LOCKS every node's sensor 
`/expose`, `/linearity` and `/camtest` answer **409 for the whole session**, on slaves as well as
the master. The slave has no session state of its own and derives one from the master's traffic:
`M`/`K` latch it (and the end of a sweep re-stamps it), `A` releases it, 120 s of silence
releases it — ⚠ `SESSION_IDLE_MS` must stay above the 60 s settle pause, which sends nothing.
- ⚠ **OTA keeps the NARROW predicate** (`g_measuring`, the ~2 s window) — "abort, then flash" is
  unchanged and `/update` is not blocked for a whole session.
- ⚠ **The idle release is a deliberate hole**: a long parked phase leaves the latch to expire
  under it. It is kept because without it a master crash would leave every slave locked until
  someone rebooted it.

## Darkness — standing rules
Every enclosure is **totally dark, LEDs off**; the entropy is readout noise. A light leak shows as
`mean_px` rising — on any node `GET /linearity` (mean_px
across exp 32/64/128/256, entry exposure restored, 409 during a session) shows `mean_px` growing
with the exposure.
- ⛔ **All nodes on PoE, permanently** (user decision).

---

## Project structure
File-local traps live next to the code: `main/CLAUDE.md` (webserver sockets, `/diagjson`,
`/expose`, PSRAM, task priority and core) and `components/*/CLAUDE.md`.
⛔ Polling the master during a measuring pass costs measurement — the HTTP task shares the consumer
core. Watch a running session on the SLAVES.
⚠ **Task priority and task CORE are load-bearing** for every task in the measuring path — read
`main/CLAUDE.md` **Resources** before creating or moving one.

All three components are **shared**: the slave repo pulls them via
`EXTRA_COMPONENT_DIRS=../elotto/components`; ota_firmware points at the single elotto_ota directory
(IDF compiles every component it discovers — `components/` would drag the camera into the recovery
image). ⚠ The repos must stay siblings on disk; build, flash and commit them together.

### Wire protocol
Commands, replies and UDP loss handling: **[components/elotto_link/CLAUDE.md](components/elotto_link/CLAUDE.md)**
(loaded when working there; read it before touching the link code in `nodes.c`).

---

## Nodes
Ten boards, all IMX219: master 192.168.178.100 and slave0…slave8. IP/MAC/COM table: skill `flash`.
**Say master/slave0…slave8, never the IP ending.**
⚠ **Two silicon revisions**: slave4 … slave8 are ESP32-P4 rev v3.2, every other node rev v1.3. Support for <3.0 and
≥3.0 is mutually exclusive, so the rev v3.2 boards run their own updater and slave images (`build_rev3`).
⚠ Discovery stops at `MAX_SLAVES` (`MAX_NODES` − 1 = 9); a further answering slave is silently ignored.
⚠ The master's UDP receive mailbox (`CONFIG_LWIP_UDP_RECVMBOX_SIZE`, `sdkconfig.defaults`) must hold
2 × `MAX_SLAVES` replies (a build-time assert in `nodes.c`); raise both together.
⚠ **Node order is DISCOVERY order**, not the slave number, and changes between sessions — the IP
per row (`/status` `nodes[]`, `/diagjson?all=1`) is what names a board; never map by position.

### Extraction — where the rate stands
Extractor rates, the cache autoload, frame quantisation and the extractor rules live in
**[components/elotto_camera/CLAUDE.md](components/elotto_camera/CLAUDE.md)** (loaded when working
there). ⛔ Never change the live extractor without `/camtest` `r10_equal` + `r10l_equal` (RAW10) / `equal` (RAW8)
true on the target.
⚠ Three different Mbit/s figures (production, consumption, used) and the `focus_win_ms`-doubles
signature: `components/elotto_camera/CLAUDE.md` **Rates**.

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

- `/update` and `POST /start` return **409** while a session runs. Abort first.
- ⚠ **Never OTA-flash while a session is running without asking the operator first.**
  Check `/status` `state` — if `running` (or not clearly idle/done/aborted), **ask** before
  aborting or flashing. Do not silently abort a live measurement to push firmware.
- ⚠ **After every OTA, poll `fw_sha` in `/status` until it CHANGES**.
  ⚠ "All nodes run the same code" is a policy, not a fact — `fw_sha` per node is in
  `/diagjson?all=1`.
- Flash commands, boot slots, a node that refuses port 80, USB recovery: skill `flash`.

## Rules
- ⚠ **Never edit sdkconfig manually.** Edit `sdkconfig.defaults`, delete `sdkconfig`, let the build
  regenerate it, verify the diff. Every project has a `sdkconfig.defaults` and sets `IDF_TARGET` in
  its CMakeLists — without both, a regenerate loses settings or fails.
- Target is always esp32p4.
- ⚠ The slave's `sdkconfig.defaults` must keep `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` **before**
  `ESP32P4_REV_MIN_0=y` — without it the build silently targets rev v3.1 and refuses to boot on
  the v1.3 boards. The rev v3 variant flips exactly this in `sdkconfig.defaults.rev3`.
- Firmware is delivered **over OTA only** in normal workflow.

---

## Where things stand
Status snapshot (history, not rules): **[docs/STATUS.md](docs/STATUS.md)**.
Closed §1.x design notes: **[docs/PLAN_HISTORY.md](docs/PLAN_HISTORY.md)** (stub → `git show 1e62bca:…`).
Former v3 PLAN: **[docs/PLAN.md](docs/PLAN.md)** (stub → `git show 4c58802:…`).
⚠ STATUS / PLAN / PLAN_HISTORY are history, not rules. Everything above this line is the rule set.
