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

- **No loops, no Runs cap, no ranking modes.** `/start` is a **whitelist**: an unknown
  key answers **400**. Deleted keys (`loops`, `runs`, `rank`, `focus`, `went`, `wruns`,
  `baseline`, `calint`, `unlimited=0`, `wpre`) keep a specific 400. Allowed: `mode`, `run`, `srun`, `gap`,
  `score`, `kdn`, `kmau`, `kser`, `kdisp`, `kbyt`, `kz`, `fix`, `maxruns`, `passes`, `confirm`, `cal`, `unlimited`, `fakepool` (TEST: `1` skips the scoring and draws a random pool). An empty key (`?=x`), a value over
  `START_VAL_MAX` 31 characters and a query string over 255 characters each answer **400** as well
  — a truncated value or query must never read as "key absent" and resolve to the default.
  ⚠ **A refused /start changes nothing**: parameters are parsed into locals and committed to
  `g_status` only after every one validates, so a 400 leaves the finished session's `/status` —
  the Z\* weights above all — describing that session. A 500 (task create failed) restores
  the snapshot, including `state`; `prefs_save()` runs only after the task exists. 100 % of the progress bar is the
  full combination space. `NUM_RUNS` 1400 is the hard cap on `results[]`.
  ⚠ **A combination space larger than `NUM_RUNS` aborts** rather than compacting mid-round.
  Eurojackpot's 7920 and 6-of-49's 5005 are both over it; the pool is sized down to `?maxruns=`.
  Compaction keeps at most 1000 rows (the ranked extremes) and the next round is appended after
  them, so `?maxruns=` is capped at `UNLIM_RUNS_MAX` 400 = `NUM_RUNS` − 1000 and a round is never
  truncated.
- **Measuring time is a session parameter, one per phase.** `?run=<s>` is the pass (per item),
  `?srun=<s>` the number scoring (per number; omitted = `?run=`); each **0,2–22 s**, `run` default
  5; out of range answers **400**, no fallback. Each phase triggers its own segment count
  (`run_segs` / `srun_segs` in `/status`). The 22 s ceiling is `EL_SEG_MAX` 2000000 divided by the measured
  rate (`EL_SEG_MAX / (RUN_SEGS_REF/RUN_MS_REF)`), not a camera limit — the rate held flat to there.
  `?gap=<s>` if present must be 0–10 s, else **400**, and applies to both phases; omitted →
  40 % of each phase's own window (`gap_s` / `sgap_s`), no floor; `?gap=0` is legal — the flush on `M`, not the gap, separates items.
  The sweep's mini-runs use the pass window; in a `?maxruns=0` session the scoring window.
  ⚠ z is normalised by √segments, so a number's scoring z and an item's z are comparable in unit;
  the scoring sums and the pass blocks are never mixed anyway. `?cal=` 0..`CAL_BUDGET_MAX_MS` (0 = no sweep),
  `?maxruns=` **0 or 10..400** (`UNLIM_RUNS_MAX`); out of range **400**, no fallback. **`?maxruns=0`
  is scoring-only**: no combination pass, the session just scores numbers round after round,
  until Abort or the 72 h limit — `results[]`, blocks, compaction, drift and the pairwise matrix are
  bypassed entirely (no pass → none of their counters move), no pool is picked (`pool_n_main/euro`
  stay 0), and the Top-10 comes from the sorted scoring table. ⚠ **`?passes=0` is the
  continuous-scoring flag and is legal ONLY with `?maxruns=0`** (else **400**): one never-ending
  scoring, one pass per round, sums **accumulate without reset** (one snap per pool, refreshed),
  whereas `?passes≥1` with `?maxruns=0` re-scores from scratch each round. Segment count follows from
  `RUN_SEGS_REF`/`RUN_MS_REF` in `sensor.h`, so nominal `?run=` matches the real
  `focus_win_ms` (short windows run a touch long by the fixed per-window onset overhead). ⚠ The
  per-node rate still varies, so `focus_win_ms` is set by the **slowest** node.
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
On Σ Z\* the rank key is the absolute sum: `high` is the largest |Σ Z\*|, `low` the smallest;
the cell and the stored sum keep the sign. Sums −12, +8, −3, +1 rank −12, +8, −3, +1.
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
  `sensor.h`: `cycle_ms ≈ 224·segments/rate + CYCLE_FIXED_MS + gap_ms`; the live ETA uses
  measured pace. ⚠ It prints the length **split into scoring and pass** and warns about nothing
   — the field moves only the pass, which at 20 scoring passes is under 1 % of a Eurojackpot
  round.

---

## The ranking channels
One ranking key, `zstar_of()` in `sensor.c`, used by the scoring pass and by `rank_key()`:

Z\* = sign(Z)·|Z/σ_z|^κ · [1 + ζ·|Disp|] · [1 + α·(1/Δn − 1)] · [1 + ε·|Ser|] · [1 + β·|Mau|] · [1 + η·|Byt|]

α β ε ζ η are `?kdn=` `?kmau=` `?kser=` `?kdisp=` `?kbyt=`, each 0..1 and independent, default 0 = that factor stays 1. κ `?kz=` 0..1 is the exponent on |Z/σ_z| (default 1; 0 = sign of Z only, the factors carry the magnitude). All five at 0 and κ = 1 is Z/σ_z. A missing column leaves its factor at 1. ⚠ The start form types each weight as a whole percent 0–100 and sends the fraction: 80 in the field is `?kdn=0.80`. `?kdn=80` is refused. A column enters Z\* only through its weight; as the pool criterion it still picks the pool and the extreme set on its own sum.
- **Z** — binomial of the camera LSBs in that window, block-centred Stouffer combine; σ_z is that block's own σ (scoring: the pass span).
- **Δn** — node agreement: σ across the contributing nodes of their block-centred z, each node divided by ITS OWN σ over that block. Independent cameras ≈ 1, so read it against 1, not against 0. Smaller than 1 stretches Z along its own sign; larger than 1 pulls Z toward 0. Δn = 0,82 and α = 1 → 1 + (1/0,82 − 1) = 1,220. Δn ≤ 0 or missing → factor 1. The cell prints the node count: `0,79 (3)`, highlighted below 0,6 and dimmed above 1,0. ⚠ **—** until the item's block has been centred, and whenever fewer than two nodes have a block σ; never 0 for "unknown" — 0 would read as perfect agreement. ⚠ Small Δn is agreement, not evidence — centring has already removed the block-wide common mode that would otherwise produce it.
- **Mau** — Maurer's universal test (NIST SP 800-22 §2.9) over the item's own window: per node, over exactly the bits z counts (`gcp_last_maurer()` in `elotto_gcp`, `,mau=`, `/camlog` `mau`), L = 8 (byte blocks, MSB first), Q = 2560 init blocks, K = the rest; z = (f_n − 7,1836656) / (c·√(3,238/K)), c = 0,7 − 0,1 + 8·K^(−3/8)/15. Then centred on that node's block mean (scoring: pass mean; a node with < 2 values stays out), Stouffer over the combined nodes, divided by the block's own σ of that combine — unit variance, read against 0, − = more compressible, coloured at |Mau| > 3. Either sign stretches, by the magnitude: Mau = −1,5 and β = 1 → 2,5; missing → factor 1. **—** until centred. ⚠ Below NIST's minimum at short `?run=` (L = 8 wants ≥ 2,07 Mbit per window); the c(L,K) correction keeps σ right, the centring removes the per-camera offset.
- **Disp** — the dispersion of the bits inside the window, built like Mau: per node the camera's per-mini-run σ over its window (3200-bit pieces; `,wsig=`, `/camlog` `wsig`), centred on that node's block mean (scoring: pass mean; a node with < 2 values stays out), Stouffer over the combined nodes, divided by the block's own σ of that combine — unit variance, read against 0, coloured at |Disp| > 3. + = the cameras' ones-rate wandered inside the window more on this item than on the rest of the block, together; − = more evenly. Either sign stretches, by the magnitude: Disp = −1,2 and ζ = 1 → 2,2; missing → factor 1. ⚠ A node publishes no window σ below 200 pieces (640 000 bit) and is then missing from the combine. **—** until centred.
- **Ser** — the serial order of the bits, built like Mau: per node the Stouffer z of its window's lag-1..4 bit agreement (`gcp_last_serial()`, `,ser=`; the four lags are the `acz` of its `/camlog`), centred on that node's block mean (scoring: pass mean; a node with < 2 values stays out), Stouffer over the combined nodes, divided by the block's own σ of that combine — unit variance, read against 0, coloured at |Ser| > 3. + = the cameras' bits repeated more on this item than on the rest of the block, together. Either sign stretches, by the magnitude: Ser = 0,8 and ε = 1 → 1,8; missing → factor 1. ⚠ The node reduces the four lags to one number; the master only centres and combines. ⚠ One camera's excursion is diluted by √k here — it stays in that node's `/camlog` (`acz`, `bchi`, `rec`). **—** until centred.
- **Byt** — the byte frequencies of the bits, built like Mau: per node the window's χ² of the 256 byte values against p^w·q^(8−w), as z = (χ² − 254)/√508 (`gcp_last_bytechi()`, `,byt=`, `/camlog` `bchi`), centred on that node's block mean (scoring: pass mean; a node with < 2 values stays out), Stouffer over the combined nodes, divided by the block's own σ of that combine — unit variance, read against 0, coloured at |Byt| > 3. + = the cameras' byte values were more uneven on this item than on the rest of the block, together. Either sign stretches, by the magnitude: Byt = 0,5 and η = 1 → 1,5; missing → factor 1. ⚠ A node sends none below 5 bytes per value (10 240 bit). **—** until centred.

- **Sync** — the cameras' synchrony INSIDE the window, published only (item hover text, Save's `Sync` column, `sync` in `/extremes`): it enters no Z\*, no sum, no ranking. Every node logs its frame pairs with their time since the flush request that follows `M` (the array's common zero, within ~1 ms) and cuts the window into `EL_SLOTS` 10 time slots of `el_slot_us(nseg)` (`elotto_link.h`; nominal window + 0,2 s over 10, at least 250 ms); a pair is split between the two slots whose centres it lies between. A slot z is against the window's own bias, so neither Z nor a node offset is in it (`camera_win_slots()`, `,sl=`). The master sums, per slot, the products of every two combined nodes' slot z, over all slots, divides by √(number of products), then centres on the block mean and divides by the block's own σ — read against 0, + = the cameras moved together. ⚠ Pass items only — a scoring row has none. ⚠ Products only, never a node's own square: one camera's dispersion is Disp. ⚠ The cameras free-run, so a pair near a slot edge sits in different slots on different nodes; the split damps that, it does not remove it.

Worked stack, all weights 1: Z/σ_z = −2, Disp = −1,2, Δn = 0,82, Ser = 0,8, Mau = −1,5, Byt = 0,5 → −2 · 2,2 · 1,220 · 1,8 · 2,5 · 1,5 ≈ −36,23.
⚠ Ser and Byt are stored per row in thousandths (±32; `run_ser()` / `run_byt()`), so the `results[]` row stays 48 bytes.

⚠ Z is divided by the σ of **that item's block**, frozen at close. There is **no** session-wide σ — `s_bsig[block].sig_p` is the only scale, and `/loops` no longer publishes it. A key with no block σ is 0, never a fallback. The product is not re-standardised.
⚠ Uncentred z ranks NODES, not items — a live table is meaningless before the first block closes.
⛔ **The key is UNBOUNDED — nothing truncates an extreme item**. Z/σ_z sits inside its block (`|Z/σ_z| ≤ (n−1)/√n`). The product of the factors does not: a small Δn, a large |Ser|, |Disp|, |Mau| or |Byt| can pass that ceiling, and a quiet block can then produce a large key. ⚠ The last round of a session is short because Abort cut it.
The formula at the top of the page leaves out a factor whose weight is 0 (at κ = 0 the base reads `sign(Z)`); the Save params file keeps all of them. A reload of a finished session shows that session's weights until an input changes.

Wire: `Z:<z>[,wsig=<σ>][,mau=<z>][,ser=<z>][,byt=<z>][,sl=<v>/…]`, all TAGGED (`,sl=`: ten slot z in hundredths, empty = none).
`,wsig=` (window σ) feeds the `Disp` column and the jump board, `,mau=` (window Maurer z) the `Mau` column, `,ser=` the `Ser` column, `,byt=` the `Byt` column; the master takes its own from `gcp_last_maurer()` / `gcp_last_serial()` / `gcp_last_bytechi()`. Every node measures the commanded `nseg`.

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
What the page shows is in `main/elotto.c`; this keeps what bites.
- The **parameter line** is built from `/status`, not the form — a curl-started run and a reloaded
  page both label themselves. ⚠ Its measured "per run" reads high early — `elapsed_ms` also
  contains the opening sweep.
- **Stat cards: the split is load-bearing** — top row round-relative in every figure, bottom row
  session-relative; the scoring has its own round-relative row.
- **Top-10** — `GET /extremes?sort=key|z|disp|nsd|ser|mau|byt&dir=high|low`, `?n=1..1000` (default 10):
  the held rows best on that column and direction, chosen on the master. ⚠ It ranks every held row
  on the master's HTTP task, which shares the consumer core: polled every **5 s**, a header click at
  most once per second — a 1 s poll stole the master's extraction CPU. ⚠ **Held rows** are the
  compaction archive (chosen on the pool criterion) plus the open round, so while the session runs
  another column's extremes are those of that archive, not of the session; after the session the
  set is every ranked pass item (the full table). Without `?sort=` the set is chosen on the pool
  criterion. `dir=` without `sort=`, an unknown value and a truncated value or query (> 95
  characters) answer **400**. A row without a value in the sorted column is not listed. The sort
  never changes the compaction archive or the pool.
- **Save** writes four files in one step (CSV for a German Excel: `;`, decimal comma, BOM):
  the pass items, `elotto_sums_…` (`/extremes?sums=1`, every number's seven signed sums for every
  scoring still in RAM), `elotto_params_….txt` (the parameters that ran, both windows, the formula
  with its weights) and `elotto_jumps_….csv` (`/extremes?jumps=1`, the whole jump board). The pass
  items once the session is over: `elotto_all<n>_…`, every ranked pass item plus the quarantined ones, from
  `/extremes?full=1&from=<i>` in pages of 2000 index slots (`span`, `next`), written last; if it
  cannot be had the held extremes are written instead. While the session runs `full=1` answers
  **409** and the file is `elotto_top…` from `/extremes?n=1000` (`crit`/`dir` in the reply and the
  file name). A failed sums fetch skips that file; a failed jumps fetch falls back to the ten
  `/status` rows.
  ⚠ The folder dialog exists only on a secure page; on the master's http address the files go to
  the browser's download folder.
  ⚠ In **scoring-only mode** (`maxruns=0`) the first file is built client-side from the sums — the
  numbers ranked by the selected criterion (`elotto_scored_<crit>_<dir>_…`).
  ⚠ A continuous scoring's true pass count is the snap's `round`, not `passes` (the per-number pass
  counter saturates at 255).
- **Camera-sigma jump board**: the largest changes in a node's own camera noise from its previous
  measurement to this one, session-wide (`WSIG_KEEP_N` 100 rows, PSRAM; `/status` `wsig` carries
  the first ten). Only a node that was in that item's combine enters it; every node's jump still
  feeds `wsig_sd`. A row's measurement values are provisional until its block is centred; a scoring
  run's are empty until that pass closes.
  ⛔ **It is a SUSPICION list, not a ranking** — the top row is the measurement whose z deserves
  the LEAST trust. It excludes nothing.
  ⚠ One row per NODE: two nodes on the same item is a shared cause, one node alone is that camera.
  ⚠ Read the **×σ** column, not the raw jump: it scales against `wsig_sd`, the session's own
  measured jump noise (~0,016 on this rig). There is deliberately **no minimum jump** — a quiet session shows ten rows
  at 2..3 σ.
  ⚠ Scoring windows enter it too, named `scoring <pass>/<round>` — not an item identity.
- **Soft-down origins**: when a block trips a node, the three measurements of that block whose z
  sat furthest from the block mean, captured at block close because compaction takes the rows one
  round later. Time is `now − (uptime_ms − t_ms)` — no RTC.
  ⚠ A trip belongs to a BLOCK, not to a measurement — σ is the spread over that round's
  `?maxruns=` items. This names what carried the spread, not what "caused" it.
  ⚠ The ×σ column is measured against that block's own spread, not the null: near 3 is
  ordinary. Read the SHAPE — one item far out is an excursion, three close together mean the
  block was simply wide.
  ⚠ The block number shown is 1-based like `/loops`; `results[].block` is 0-based.
- **The start form remembers its last values** (NVS, survives reboot and OTA). ⚠ **Only a start
  carrying `confirm=1` writes them** — i.e. only the web UI. A curl start that omits `kdn` /
  `kmau` / `kser` / `kdisp` / `kbyt` runs at 0 and does not overwrite the stored form weights. ⚠ It changes
  **no** API default: an omitted parameter still resolves to the compiled-in value. Mode is not
  remembered — it is which button was pressed. The pool criterion is remembered by its own rule
  (written on the Σ click).

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

## Current-item display
A "Now:" card shows the number being scored or the combination being measured, in large type.
The session is always unattended.
- The card updates ~70 ms **before** the bits start ⛔.
- `GET /focus` (~60 B) polled at 10 Hz, separate from the 2,5 KB `/status`; `seq` is monotonic per
  window, so the UI counts *missed* windows — a skipped window names the wrong numbers.
- `POST /pause?on=1|0` holds **between** runs only; state stays `running`, paused time is excluded
  from `elapsed_ms`.
- **Every window starts on fresh bits** (`onset_settle()`, all nodes flush in parallel on `M`;
  the first pair after `M` is discarded unextracted)
. ⚠ A flush that does not finish **voids the run** (`flush_timeouts`).
- ⚠ `?focus=` answers **400**.

---

## Camera calibration (per BLOCK)
At every insertion the master broadcasts `K<budget_ms>,<segs>`, sweeps its own ladder in parallel,
and waits for every node's `OK:`. Each node keeps the rung with the **lowest `raw_sigma` among
candidates that clear the gates**, falling back to the bare gate if none qualify.

⛔ **The bias gates nothing and selects nothing**. It is non-stationary per node on a
timescale of minutes, only the SHAPE across a ladder is stable, and the centring subtracts it
anyway. It stays measured and published. ⛔ Do not fit an absolute bias bar — one certified-empty a
healthy node.
⚠ **The incumbent rung is KEPT** unless a challenger beats it by `CAL_KEEP_MARGIN_K` 3 SE of its
own measurement (`kept` in `/calibrate`); for σ that SE is σ/√(2(m−1)) over m mini-runs.
⚠ **The σ key is coarse**: at the 10 s default budget 3 SE is ~5 %, so a smaller gain will not move
a rung. SE goes as
1/√budget, so `?cal=40000` halves the bar to ~2,4 %. The incumbent is what runs when the sweep STARTS, so a
manual `/expose` gets one sweep of protection — deliberate.

**An IMX219 does not ladder**: exposure fixed at 1600 lines from boot, the sweep is a health check that
never moves the rung, with no light gate (no DARK/ZDIFF). Its details, the gate thresholds,
`raw_runs_z` and the per-rung settle: `components/elotto_camera/CLAUDE.md` — read it before
interpreting `/calibrate`.

- The budget is a **cap, not a target** (default 10 s, `?cal=<ms>`). ⚠ `?cal=0` turns every sweep
  off; a soft-down trip then waits until the next session. **When** is the dynamic interval
  (`calibrate_all()`): the session-start sweep always runs; after it a sweep runs at the next
  candidate point — the end of every scoring pass, before the pass, the round boundary — once the
  interval has run out: `CAL_DYN_MIN_MS` 15 min, doubled by every ok sweep (all nodes certified, no
  exposure moved) up to `CAL_DYN_MAX_MS` 2 h, back to 15 min after any other sweep or a soft-down
  trip (`calibrate_shorten()`). `/status` `cal_interval_ms` / `cal_due_ms`; `cal_did_sweep` / the
  `/loops` `cal_ms` mean "a sweep ran in this round". The page's **Log** card shows every sweep,
  change, pause and soft-down with wall time (`GET /loops?ev=1`).
- ⚠ **A rung change is not free**: after one, this rig's cameras need ~1 min to settle (px still
  climbing, bit bias moving by ~0,002), and the items measured in that minute sit far enough from
  the block mean to trip soft-down on their own. So **any sweep that moves any node's exposure is
  followed by a 60 s settle pause of the whole array** (`CAL_SETTLE_AFTER_MS`), nothing measured; a
  sweep where every node kept its rung pauses nothing. It fixes only the trips that follow a change.
- **Nodes land on different exposures on purpose**; what they must share is the segment count.
- `GET /calibrate` serves the whole last sweep per rung with the gate each failed plus the raw
  stats, **on every node** — a per-node optical fault is diagnosable. The chosen setting is
  recorded per block in `/loops`.
- **`/loops` also carries `cam_sig` / `cam_rsig` (stream σ) and `cam_px`**.
  ⚠ **They are cumulative since the last SWEEP, not per block**  — `camera_stats_reset()`
  runs only in the sweep and on `/expose`. The sweep before the pass resets them, so a
  block's row spans **that block's own pass** — a short span and therefore a noisy σ. ⚠ A `?cal=0` session has no reset at all. For anything that has to
  be located in time use **`GET /camlog` on the node**  — never these. `/camlog` is the
  record that does not depend on the master keeping the row.
  ⚠ `cam_px` 0 = the node did not report it, **not** a dark frame.
  ⚠ Every other `cam_*` field is what the last SWEEP found: two blocks on one setting carry an
  identical `cam_bias` and describe neither. That is why a σ 6,94 block was unattributable.
  ⚠ `cam_rsig` is recorded and never gated — no null of 1, non-stationary per node.
- ⚠ `camera_get_stats()` is cumulative since the last sweep; in a `?cal=0` session they are
  lifetime averages.
- ⚠ Never price monitor cost with `/camtest` — the delivered bit rate is the number.
- ⚠ **Never judge `raw_sigma` from single `/expose` readings.** It is non-stationary per node
  on a timescale of minutes: two readings at IDENTICAL settings gave 1,211 and 1,025. Only the
  SWEEP compares — the whole ladder back to back.
- ⚠ **The sweep ladders the EXPOSURE only.** `camera_calibrate()` passes the gain in force at entry
  (`g0`) to every rung, so `CONFIG_ELOTTO_CAM_REG_GAIN` is the operating point for the life of
  the board, not a boot value — and a `POST /expose?gain=` override is carried forward by the
  next sweep instead of being replaced.
  ⛔ **And it stays that way** — gain 1023, exposure ladder only.

---

## The per-window log — `GET /camlog` 
**Every node keeps its own**, 512 windows deep, one entry per measurement window. It is the only
place a disturbance can still be located in time.
- ⚠ **It is a RING** — ~25 min at 2,9 s per window. `dropped` counts what fell out, so a gap
  never reads as a quiet stretch. **Pull it inside the session, per node**; there is no
  master-side collector.
- ⚠ `t_ms` is each node's OWN uptime. Align by `tag` or by ordinal, **never** by subtracting
  timestamps across boards.
- Entry fields, the `rec` session record and what the log holds beyond the wire: skill `camlog` —
  read it before interpreting a reply.

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
- **main/elotto.c** – app_main, Ethernet, webserver, HTML/JS UI.
  ⚠ The URI-handler cap fails silently (404, return value unchecked) — the count lives at
  `start_webserver()`; prefer `?all=1` on an existing endpoint over a new handler.
  ⚠ **`max_open_sockets` 13 (lwIP 16 minus the 3 httpd reserves) exists so the UI cannot lock an
  external client out**. With a smaller table one open page fills it, and `lru_purge_enable`
  then evicts each newly accepted, still-silent client before its request is read — `curl` sees a
  successful TCP connect followed by an immediate close, every time, while the page keeps updating.
  ⚠ That failure looks exactly like a dead HTTP task. **Open a second browser window**: if it
  loads, the board is fine and the caller is being evicted.
  ⛔ It fixes the lock-out, **not the load**: the HTTP task shares the consumer core, so polling the
  master during a measuring pass costs measurement. Watch a running session on the SLAVES.
  **`GET /diagjson?all=1` is the COLLECTOR**: the whole array's front-end health in one request,
  discovery order, IP per row. 409 while measuring. ⚠ `cal_*` is what the last SWEEP chose;
  `exposure`/`gain` are LIVE — they differ after a manual `/expose` or an uncertified sweep.
  `POST /expose?exp=<lines>[&gain=<g>]` sets one node's live operating point (trying a setting
  against a live reading; resets camera stats so `mean_px` answers in ~2 s). Not sticky —
  the next sweep overwrites it, correctly. 409 while measuring.
- **main/sensor.c/h** – GCP analysis, scoring and the pool, the pass, blocks, centring, drift,
  soft-down, publishing. ⚠ Counter and round-base semantics are commented at the definitions in
  `sensor.h`.
- **main/nodes.c/h** – the array: UDP link, discovery, calibration handshake, per-node health,
  drop/reboot policy. sensor.c reaches other boards only through `nodes.h`.
- **main/focus.c** – current-item card, pause, run gap, session clock — one file because they share state.
  Also the event log ring (`evlog()`, 48 entries, PSRAM) behind the Log card.
- **tools/tune.html** – live per-node exposure/health board (`/linearity` + last sweep). Open in a
  browser on the operator PC; 409 while a session runs. Not served by the master.
- **ota_firmware/** – the network updater, its own IDF project. Ethernet + HTTP + esp_ota only.
- **components/elotto_camera/** – OV5647 / IMX219 entropy extraction. One stream; the runs half
  is armed only inside a sweep. Also serves `/camlog`, `/linearity`, `/expose`,
  `/calibrate` and `/camtest` for **every** node from one implementation — the boards must
  not describe their own cameras in different shapes.
- **components/elotto_link/** – the UDP wire format (`EL1 <seq> <payload>`, ports 5000/5001), plus
  `EL_SEG_MIN`/`EL_SEG_MAX` as ONE definition for both firmwares.
- **components/elotto_gcp/** – the z primitive (`gcp_zscore_raw()`, `gcp_z_per_bias()`), shared so
  the nodes cannot disagree about what a z is. ⚠ The remaining soft-float calls per segment are deliberate.
- **components/elotto_ota/** – update endpoint + boot safety (`/update` `/boot` `/reboot` `/poison`
  `/otainfo`; `BOOT_ATTEMPT_LIMIT` 3, `HEALTHY_UPTIME_MS` 30000). ⚠ `fw_boot_attempts` counts
  boots since the last run that stayed up 30 s, **not** failures — every node reads 1 for its
  first 30 s after a flash.

All three components are **shared**: the slave repo pulls them via
`EXTRA_COMPONENT_DIRS=../elotto/components`; ota_firmware points at the single elotto_ota directory
(IDF compiles every component it discovers — `components/` would drag the camera into the recovery
image). ⚠ The repos must stay siblings on disk; build, flash and commit them together.

### Wire protocol
Commands, replies and UDP loss handling: **[components/elotto_link/CLAUDE.md](components/elotto_link/CLAUDE.md)**
(loaded when working there; read it before touching the link code in `nodes.c`).

### Resources
- **PSRAM is mandatory**: capture buffers, the LSB-ones side ring (`s_ring_raw`), `loop_hist`,
  per-item per-node archives (`s_node_z` and the Mau / Ser / Disp copies, sized to `NUM_RUNS`),
  the scoring-sum log (`SCORE_SNAP_MAX` 128 snaps, ~225 KB) and the camera-jump board
  (`WSIG_KEEP_N` 100 rows, ~7 KB), and the full table (`FULL_MAX` 250000 rows, ~13 MB of the
  ~22 MB free in a session).
  The word ring `s_ring` (64 KB) is **internal RAM**; `results[]` stays internal too:
  `NUM_RUNS` 1400.
- ⚠ **The measurement path reads the ring in BLOCKS** — `camera_read_words()`, 112 words per call
  from `gcp_zscore_raw()`. `camera_read_word()` takes `s_mutex` and fences four times per
  32 bits; read that way, the reader — not PSRAM, not the camera — sets the session rate. Single words are for seeding only.
- ⚠ **Task priority is load-bearing**: any task calling `camera_read_word[s]()` must run **above** the
  extraction task, or the consumer starves ~10× (see `camera.h`; signature: ring `drops` huge with
  `waits == 0`).
- ⚠ **Task CORE is load-bearing too, and it is a separate rule.** Every task in the measuring path
  is created with `xTaskCreatePinnedToCore()`: extraction on `ELOTTO_CAM_TASK_CORE`, consumer and
  web server on `ELOTTO_CAM_CONSUMER_CORE` (both in `camera.h`). Plain `xTaskCreate()` is
  `tskNO_AFFINITY`, and the master creates `elotto_task` fresh on **every** `/start` — unpinned it
  shares a core with `cam_task` in about one session in three and **everything halves**.
  ⚠ The signature is consumption halving EXACTLY while all other nodes stay normal.
  ⚠ **One fast session proves nothing here** — it was intermittent. Re-test any new task in this
  path over ~20 session starts.

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
- ⚠ **`mbit_s` is PRODUCTION, `consume_mbit_s` is what a measurement READ**. The first
  counts words the ring then discarded, so a slow consumer reads HIGH on it. `/diag` shows the
  second; use the first only for the sensor ceiling  and `/camtest`. The node table's
  Mbit/s is a THIRD figure, the **used** rate: bits that entered the node's z per second of session
  time (`z_n` × segments × 224 / `elapsed_ms`, segments = `srun_segs` and `run_segs` weighted by
the session's `win_score` / `win_pass` window counts); the
  other two are in its hover text.
- ⚠ **If `focus_win_ms` ever doubles** (~5,13 s → ~10,2 s at identical parameters), suspect an
  unpinned task in the measuring path, not the camera  — signature: consumption halves
  exactly on that node alone. The rule is under **Resources**; this is how it surfaces here.

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
