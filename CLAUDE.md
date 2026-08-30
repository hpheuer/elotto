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

## Concept
Four-node ESP32-P4 array. The master scores lottery numbers via GCP methodology; up to three slaves
measure the same window in parallel, triggered by one UDP broadcast (port 5000). Slaves are
discovered by broadcast at every session start — no IP table, no node count configured. Combined
z = **Σ z_node / √k** over the k nodes that answered *that* run, so a missing reply costs that
run's gain, not the session.

**Entropy is photons, and only photons** (user decision). One OV5647 per node, never shared.
Non-overlapping frame pairs, diff = f[2k+1]−f[2k] per pixel (cancels FPN exactly), LSB packed,
XOR-folded. ~5,7 Mbit/s per node idle, ~3,7 under load `[D25]`.
⛔ The on-chip TRNG is deleted from both firmwares — a whitened hardware RNG would be
indistinguishable from the real thing in every statistic this project computes; this covers the
LFSR-fed-from-camera variant (DECISIONS "Dropped"). The Fisher–Yates order uses an xorshift32 PRNG
seeded from the camera; it never enters a z.

**The ×√n gain is NOT established** — it assumes node independence. Judge a session on per-block
combined σ **and** the full pairwise matrix, never on `pair_r` alone: **σ, not correlation, is
where this array fails** (measured: σ 1,378 with every pairwise |r| ≤ 0,024).

Modes: Eurojackpot (5 of 50 + 2 of 12, 7920 combinations) and 6 of 49 (5005).
⚠ `?mode=` is `1` for 6-of-49 and **anything else** for Eurojackpot — it tests `val[0]=='1'`, so
`?mode=649` silently starts a Eurojackpot session.

---

## v3 — the single-pass session
**PLAN.md §2 is the contract.** Every combination in the confirmed pool is measured **exactly once**,
in one Fisher–Yates random order, with **one continuous window per Focus item** — scoring and
measurement share the same length.

- **No loops, no Runs cap, no ranking modes.** `?loops=`, `?runs=`, `?rank=`, `?baseline=` answer
  **400**. 100 % of the progress bar is the full combination space. `NUM_RUNS` 7200 is the hard cap —
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
- **Blocks are the statistics unit.** Every `cal_interval_ms` (default 15 min, `?calint=`, 0 = none)
  the pass parks for the camera sweep; the boundary closes a block → `/loops` row, drift point,
  pairwise fold, block centring.
- Pause stops the clock; Abort publishes the measured prefix.

### Phases
**Phase 0 — scoring.** Each number 1..N gets **exactly one long run** (the session window) in a
fresh Fisher–Yates order. Direction pre-registered: `?score=high|low|abs`, default `high` — it only
picks the pool. **The scoring key is the pass key — z_ctr + optional pre-fold, at the session's
`?wpre=`** `[D48]``[D53]`; `score_build_keys()` is the only place a scoring key is built.
⚠ The scoring pass centres and standardises itself per channel, on its own candidates — no block
mean exists yet; for the **pre-fold** channel this is the difference between working and not `[D48]`.
⚠ `?wpre=0` still reproduces the pure-z order exactly.
⚠ `?went=` answers **400** — the spectral-entropy channel is deleted `[D53]`.
⛔ Never repeat a target in place, in any form `[D5]`.

⛔ **Phase 1 — the baseline — is DELETED** (user) `[D48]`: block centring computes the same drift
reference from ~200 runs instead of 10. ⚠ The slave firmware still answers `B`; nothing sends it.

**Phase 2 — the pass.** One Fisher–Yates order over the whole space, each item measured once.
`PairAcc[i][j]` accumulates per-block-centred moments → full Pearson matrix + per-node σ in
`/status` (flagged if |r|·√n > 3). `close_block()` → `record_loop()` stores per-block offsets/σ and
camera health; `drift_add()` regresses the per-block mean on the block index → `drift_slope`,
`drift_t`; |t| > 3 flags real drift.

### Two attended gates, opt-in on `POST /start?confirm=1`
The web UI always sends it; curl never does.
- **`PHASE_READY` — the observer gate.** Parks after the sweep, released by `POST /ready`, then 1 s
  dark. No timeout, by design. ⚠ Armed on `focus_mode`, not on `confirm` alone `[D6]`.
- **`PHASE_POOL_CONFIRM`.** `POST /pool?act=ok|more|cancel&main=..&euro=..`. "Select more" re-scores
  with the still-checked numbers omitted, so they keep the measurement that chose them. Keeping
  exactly 5+2 = ONE combination is intended and the highest-power use. 15-minute timeout accepts
  unchanged (`pool_auto=1`); at `focus=0` it accepts immediately, same flag.

### Unlimited Mode — rounds instead of one pass
UI checkbox + **Runs per round** (default 100); `POST /start?unlimited=1&maxruns=<n>`. A **round** =
score every number → keep as many of the best as fit `maxruns` runs → measure that space once in a
fresh Fisher–Yates order → score again. Ends only on **Abort**; the `results[]`-full stop survives
as backstop for a compaction that cannot allocate.

- **Pool sizing maximises COMBINATIONS MEASURED, nothing else**; the bonus preference is only the
  tie-break `[D3]`.
- **Results ACCUMULATE** — `results[]` is never cleared between rounds; every statistic runs on the
  union of all rounds.
- **Every unlimited round compact at the boundary** `[D56]`: the 100 most extreme items by
  `|rank_key|` stay as rows (both tails, so Top-5/Bottom-5 stay exact). The rest folds into
  moments — pass mean/σ/χ² stay exact. A single pass only compact if the space
  would not fit uncompacted `[D42]`.
  ⚠ The counter and round-base semantics after a compaction (`completed`/`runs_completed`,
  `round_base`/`round_item_base`) are subtle and documented at their definitions in `sensor.h` —
  read them before touching anything that counts or indexes items; getting them wrong is what broke
  the first compaction on hardware `[D42]`.
  ⚠ `compacted=` non-zero in the CSV header: the rows are extremes plus survivors, not a sample —
  never compute a distribution from them.
- **Every round closes its own block**, even at `?calint=0`, so centring never mixes items from
  either side of a re-scoring. Rounds after the first re-run the sweep **before** scoring (skipped
  at `?calint=0`).
- **No pool-confirmation gate** (`pool_auto=1`); the observer gate still fires once at start.
- ⚠ Inside a round "measured exactly once" holds; **across rounds a combination can recur**. Each
  recurrence is its own row; the identity is **(round, index)**.
- A truncated round draws a **random subset**; only the last round truncates `[D4]`.
- `/status`: `unlimited`, `runs_cap`, `round`, `round_base`, `round_done`, `round_total`,
  `round_start_ms`. `completed` is session-wide, `total` the CURRENT round — ⚠ a progress bar must
  use `round_done/round_total`. `round_start_ms` stamps when MEASURING began (sweep and scoring
  excluded); 0 outside unlimited mode.
- `/status` publishes `pool_main`/`pool_euro` for the whole running session. ⚠ They are withdrawn
  at the ROUND BOUNDARY and before RUNNING, not at the scoring pass — deliberate (stale numbers
  would stand through the insertion and the opening sweep).
- The **Runs per round** field previews pool size and round length from the rate model in
  `sensor.h`: `cycle_ms ≈ 200·segments/rate + CYCLE_FIXED_MS + gap_ms` `[D39]`; the live ETA uses
  measured pace.

---

## The ranking channels
One ranking key from folded z plus BOTH pre-fold channels `[D45]``[D56]``[D58]`:
**key = ((1−p)·z_ctr + (p/2)·zp_ctr/σ_p + (p/2)·zc_ctr/σ_c) / √((1−p)² + 2·(p/2)²)** —
p = `?wpre=` splits folded against pre-fold, the pre-fold half splits evenly between the two.
`?score=` only picks the pool.
⚠ **Each pre-fold channel is standardised by ITS OWN σ** — `rank_sig_p` for `zp_ctr`,
`rank_sig_c` for `zc_ctr`. They differ by roughly a factor of two; one shared σ reweights the
key silently `[D58]`.
⚠ The √ normaliser assumes independence and the two pre-fold terms share their bits, so at
p → 1 the key's variance runs above 1. Scale only — Z* studentises on measured `rank_sigma`.
⚠ **`?went=` answers 400** — spectral entropy deleted `[D53]`.
⚠ **`?wruns=` answers 400** — NIST-runs ranking deleted `[D55]`.
⚠ **The pre-fold channels RANK, they do not TEST**: tables use `rank_key()`. There is no
Bonferroni on folded z `[D57]`. UI tables show **Z-Pre** (`zp_ctr`) and **Conc** (`zc_ctr`)
beside Z* — since D58 both of them also rank, each with half of `pre_w`.

**Concordance (D56).** The second pre-fold channel, robust where `zp_ctr` is sensitive.
Per node, split the pre-fold window at nseg/2. If both halves have the
same sign, that node's contribution is `√2 · min(|h1|,|h2|)` with that sign — equal to the
full-window z when the bias is stable. Opposite sign or a zero half → 0. Then drop the
loudest node and Stouffer-combine the rest. A single bright-rung offset cannot own Top-5;
a 50 ms glitch in one half cannot either. k < 2 after the drop → 0.

**Folded z** — archive and p-values. ⛔ The fold stays: without it there is no stable null `[D17]`.

**Pre-fold z** — `?wpre=<0..1>`, API default **0**, FORM pre-fill 0,8 (user, 2026-08-27). The fold
suppresses a mean-bias effect by √2·ε — ~7000× at ε = 1e-4 — which is the quantity a GCP experiment
looks for, so the unfolded stream is scored, combined, centred and archived beside z `[D45]`.
- ⚠ Uncentred it ranks NODES, not items (per-node raw offsets ran to −47σ) — a `wpre>0` live table
  is meaningless before the first block closes `[D45]`.
- ⚠ At `wpre=1` the folded z has weight **zero** in the key: the tables are then ranked with no
  contribution from the channel the p-values come from.
- `ENT_Z_CLAMP` 12 bounds **both** pre-fold terms **in the key only** — the archive is never
  clamped; an item pinned in either channel counts once in `pre_clamped` `[D58]`.
- **The archive carries zp_ctr and zc_ctr even at weight 0** — the weight enters nothing but the ranking.
- ⚠ **`pre_w` splits the pooling table for the TABLES only** — `z_raw`/`z_ctr` pool across weights.
- Normalised as the plain binomial over the bits actually consumed (it covers more bits than the
  folded z in the same window).

Wire: `Z:<z>[,nan,<z_pre>[,<h1>,<h2>]][,wsig=<σ>]` — pre-fold past the second comma; half-window pre past the
third/fourth `[D56]`; `,wsig=` is TAGGED and appended, the node's camera σ over that window `[D62]`. Middle slot held with `nan` (was H; ignored `[D53]`). A node without the raw
ring omits the trailing fields. M does not travel: every node measures the commanded `nseg`.
⚠ `CAM_RAW_EVERY` is gone — a measurement channel is not sampled `[D45]`.

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
  block badge, Save CSV. The nearest-zero table is deleted `[D56]`. Bonferroni
  is deleted `[D57]`. Columns: `Z*`, `Z-Pre`, `Conc`.
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
- CSV is **German**: `;` separator, `,` decimal — a decimal point makes Excel read text.
- **The start form remembers its last values** (NVS, survives reboot and OTA): measuring time,
  focus, unlimited + runs per round, direction, pre-fold weight. `/` serves them as a script chunk
  appended to the page `[D49]`. ⚠ **Only a start carrying `confirm=1` writes them** — i.e. only the
  web UI. A curl start sends no `wpre=` and would otherwise replace the operator's weight with the
  API default. ⚠ It changes **no** API default: an omitted parameter still resolves to the
  compiled-in value, so every control arm is unaffected. Mode is not remembered — it is which
  button was pressed, not a field. No entropy weight `[D53]`, no runs weight `[D55]`.

### CSV header
`# elotto v3 mode= focus= score= items=<measured>/<planned> ranked= excl= void= blocks= paused_ms=
pass_* v_eff= flush_timeouts= drift_t= unlimited= runs_cap= rounds= run_s= run_segs=
gap_s= compacted= pre_w= pre_n= pre_clamp= pre_sig= conc_sig= rank_mean= rank_sigma=
fw=<version>/<elf sha>`, then `# nodes=<ip list, discovery order>` and
`# fw_nodes=<sha per node, same order>` (`?` = never answered).
`?all=1` appends a **`round` column last**, then `key;zp_ctr;p0..p3;zc_ctr;w0..w3`, so older parsers still line
up on the columns before `round`. `w0..w3` are the per-node CAMERA σ of that item's own window
`[D62]` — the instrument's noise while the z beside it was taken; empty = not reported, and a
quiet window reads 1,0, never 0. `p0..p3` are per-node RAW pre-fold values in discovery order;
empty means no report, **not** zero. `zc_ctr` is the concordance z `[D56]`. (`zh_ctr` / `h0..h3` / `ent_*` are gone `[D53]`. Nearest-zero group is gone `[D56]`.)
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
| `pre_w` (2026-08-26) | one weight — for the TABLES. `z_raw`/`z_ctr` pool across weights `[D45]` |
| scoring key 2026-08-28 | one side — a pool chosen before it was chosen on a different key `[D48]` |
| spectral channel deleted 2026-08-28 | post-D53 only — no `ent_w` / z_h `[D53]` |
| runs ranking deleted 2026-08-29 | post-D55 only — no `wruns` / zr `[D55]` |
| concordance / half-window ranking 2026-08-29 | post-D56 only — tables use `zc_ctr` `[D56]` |
| both pre-fold channels rank 2026-08-29 | post-D58 only — D56 sessions ranked on `zc_ctr` ALONE `[D58]` |
| v3 vs any v2.x | v3 only |

Unlimited-mode data carries two more: split on `round` before pooling with a single-pass session,
and decide what to do about combinations that recur across rounds before pooling rounds together.

---

## Pass-level health and node health
Under H₀ with a working instrument: mean ≈ 0, σ ≈ 1, Σz² ≈ n. **Ranking is secondary: read
`pass_mean`/`pass_sigma`/`pass_chi2`, `drift_t` and the pairwise matrix before any table.**

⛔ **`null_flags`, the `NB` attribution counter and the per-node CUSUM are DELETED** (user) `[D47]`.
**The software publishes the numbers and draws no verdict from them**; `PAIR_FLAG_T`/`DRIFT_FLAG_T`
survive as display hints on `/diag` and gate nothing. ⚠ The judgement the old banner made is still
true when it would have fired — it is now a human reading `pass_sigma` and `drift_t`.
⚠ **Soft-down watches the FOLDED per-node σ.** At `?wpre=` 0,8 the ranking rides the pre-fold
stream, whose σ no gate watches. Deliberately not fixed in software; the answer is the sweep and
the light `[D47]`.

**Soft-down** (`nodes[].soft_down`) takes a node out of the combine after a block with
σ > `NODE_SIGMA_SOFT` (1,25). Sticky; clears after `NODE_SOFT_CLEAR_BLOCKS` (4) blocks under the
peer-referenced bar (`clear_sig`, published per block in `/loops`) `[D12]`. It never reboots
anything.
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

## Focus display
A "Focus:" card shows the current target in large type — the observer is present while the noise is
sampled (GCP/PEAR protocol). Statistically the session is merely **tagged**: `?focus=1`, `"focus"`
in `/status`, `# focus=on|off` in the CSV.
- The panel lights ~70 ms **before** the bits start, by design ⛔ `[D33]`.
- `GET /focus` (~60 B) polled at 10 Hz, separate from the 2,5 KB `/status`; `seq` is monotonic per
  window, so the UI counts *missed* windows — a skipped window is mislabeling, not blur.
- `POST /pause?on=1|0` holds **between** runs only; state stays `running`, paused time is excluded
  from `elapsed_ms`.
- **Every window starts on fresh bits, attended or not** (`onset_settle()`, all four nodes flush in
  parallel on `M`) `[D34]`. ⚠ A flush that does not finish **voids the run** (`flush_timeouts`)
  `[D35]`.

---

## Camera calibration (per BLOCK)
At every insertion the master broadcasts `K<budget_ms>,<segs>`, sweeps its own ladder in parallel,
and waits for every node's `OK:`. Each node keeps the rung with the **lowest pre-fold
|raw_bias − 0,5| among candidates clearing the σ gate with margin**, falling back to the bare gate
if none qualify `[D16]``[D46]`.

⛔ **The key is the PRE-FOLD bias and it is never a gate** `[D46]`: after the fold every certified
rung sits below the window's own sampling error (the folded key once picked a rung whose real
offset was 10× worse, by a fortieth of its own SE). The pre-fold statistics are non-stationary per
node on a timescale of minutes; only the SHAPE across a ladder is stable — it selects, it does not
certify. ⛔ An absolute pre-fold bar was fitted and certified-empty a healthy node; do not re-fit
one.
⚠ **The incumbent rung is KEPT** unless a challenger beats it by `CAL_KEEP_MARGIN_K` 3 SE of its
own measurement (`kept` in `/calibrate`). The incumbent is what runs when the sweep STARTS, so a
manual `/expose` gets one sweep of protection — deliberate.

Gates a rung must clear: autocorr < `CAL_AUTOC_TOL` 0,01 (⚠ never subsample — it gates),
|σ−1| ≤ 0,05, pre-fold σ ≤ `CAL_RAW_SIGMA_K` 1,35 × the ladder's own best (**relative,
one-sided**, after the whole ladder `[D46]`), no stuck frames, `mean_px` ≥ 5,0 `[D18]`,
`zero_diff` ≤ `CAL_MAX_ZERO_DIFF` 0,125. ⛔ Folded bias and `CAL_MAX_MEAN_PX` 100 are
**publish-only** since 2026-08-28 — they no longer fail a rung `[D52]` (`[D19]``[D20]`).

- **The dark end is gated because photons do the whitening** `[D18]`. Dim the lamp and more rungs
  fail; the answer is light, not a lower floor.
- **The pre-fold runs statistic is published per rung and gates nothing**; armed only inside a
  sweep — permanently on it cost measurable bit rate `[D46]`. ⚠ `raw_runs_z` 0,0 in a measurement
  window means NOT ARMED, not "perfectly random" (`raw_trans` says which).
- ⛔ **No fold trial.** The XOR fold is permanently on `[D17]`.
- The budget is a **cap, not a target** (default 10 s, `?cal=<ms>`, 0 = off) `[D21]`. **The trigger
  is TIME** (`?calint=`, default 15 min, 0 = none) — it also sets the drift regression's resolution
  and the block size.
- **Nodes land on different exposures on purpose**; what they must share is the segment count.
- `GET /calibrate` serves the whole last sweep per rung with the gate each failed plus the raw
  stats, **on every node** — a per-node optical fault is diagnosable. The chosen setting is
  recorded per block in `/loops`.
- **`/loops` also carries `cam_sig` (folded σ), `cam_rsig` (pre-fold σ) and `cam_px`** `[D50]`.
  ⚠ **They are cumulative since the last SWEEP, not per block** `[D62]` — `camera_stats_reset()`
  runs only in the sweep and on `/expose`. Up to three blocks share one accumulation here, and a
  block right after a sweep has a shorter one and therefore a noisier σ. For anything that has to
  be located in time use the **per-item** `w0..w3` in the CSV, not these.
  ⚠ Every other `cam_*` field is what the last SWEEP found: two blocks on one setting carry an
  identical `cam_bias` and describe neither. That is why a σ 6,94 block was unattributable.
  ⚠ `cam_px` 0 = the node does not report it (slave older than 2026-08-28), **not** a dark frame.
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
- ⚠ **Too much light on one node is a fault, not a luxury**: it forces the sweep onto the short
  rungs, which certify worst. slave0 went from 3 of 9 rungs at `raw_sigma` 1,36 to 6 of 9 at
  1,032 after ~6,7× dimming `[D59]`. Judge it by illumination per exposure unit (`mean_px`/`exp`),
  not by `mean_px` alone. ⚠ **slave1 is currently the bright one** (0,48 against 0,13..0,22).

---

## Illumination — standing rules
The enclosure is **LIT, not dark** `[D28]`.
- ⛔ **Never power illumination from a node's VSYS pin** `[D29]`.
- ⚠ **After physical work, let the light settle ~30 min** before a long run `[D30]`.
- ⚠ **Do not judge the light by one `mean_px` reading** — sweep, or take a time series.
- ⛔ **All four nodes on PoE, permanently** (user decision) `[D31]`.
- ⛔ **Do not switch camera hardware** on the theory that the OV5647 is the problem `[D32]`.

---

## Project structure
- **main/elotto.c** – app_main, Ethernet, webserver, HTML/JS UI. Endpoints: `/` `/status` `/start`
  `/abort` `/loops` `/results.csv` `/focus` `/pause` `/calibrate` `/pool` `/ready` `/probe`
  `/expose` `/diag` `/diagjson` `/camtest`, +5 from elotto_ota. (`/spectest` `/specdump` deleted
  with the spectral channel `[D53]`.)
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
- **main/focus.c** – focus panel, pause, run gap, session clock — one file because they share state.
- **partitions.csv** – shared table (factory 1 MB + ota_0/ota_1 3 MB on 32 MB flash); a board
  flashed by one project must be updatable by the others.
- **ota_firmware/** – the network updater, its own IDF project. Ethernet + HTTP + esp_ota only.
- **components/elotto_camera/** – OV5647 entropy extraction. **`raw_bias`/`raw_sigma` are the
  PRE-FOLD stream** (`cam_raw_t`) — everything else published is post-fold `[D43]`; the runs half
  is armed only inside a sweep `[D46]`. ⚠ With the fold OFF, raw and folded pairs must agree
  exactly — a divergence is a bug, not a finding.
- **components/elotto_link/** – the UDP wire format (`EL1 <seq> <payload>`, ports 5000/5001), plus
  `EL_SEG_MIN`/`EL_SEG_MAX` as ONE definition for both firmwares.
- **components/elotto_gcp/** – the z primitive (`gcp_zscore_raw()`, `gcp_z_per_bias()`), shared so
  the nodes cannot disagree about what a z is `[D37]`. (`gcp_spec.c` deleted with the spectral
  channel `[D53]`.) ⚠ The remaining soft-float calls per segment are deliberate `[D26]`.
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
`R` reboot. Replies `OK`, `Z:<z>[,nan,<z_pre>[,<h1>,<h2>]]`, `D:`, `E:<reason>`, `V:<reason>` (void, not a
fault). `h1`/`h2` are pre-fold z of the two halves of the same window `[D56]`; a slave without them
is ranked on full pre (leave-one-out still applies). Extra fields ignored.
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
**5,71 Mbit/s per node idle** (98,5 % of what the sensor can deliver `[D23]`), **~3,7 under load**,
where the loop is compute-bound `[D25]`.
- ⛔ Nothing done to the extraction path can raise the **idle** rate `[D23]`; the loaded rate is
  still open — prove any change with `ms_extract` under load, never at idle `[D25]`.
- `/diagjson` publishes the per-pair split on every node: `ms_pair` = `ms_wait` + `ms_extract` +
  `ms_rest`.
- ⚠ **`mbit_s` is PRODUCTION, `consume_mbit_s` is what a measurement READ** `[D60]`. The first
  counts words the ring then discarded — it read 5,71 on the master against 3,34 on the slaves
  purely because the master's consumer was slower and its ring overflowed. The node table and
  `/diag` show the second; use the first only for the sensor ceiling `[D23]` and `/camtest`.
- ⚠ **The window length WAS bimodal; the cause was unpinned tasks and it is fixed** `[D61]`.
  `focus_win_ms` was either ~5,13 s or ~10,2 s per 5 s run at identical parameters, because
  `elotto_task` is created on every `/start` and, unpinned, shared a core with `cam_task` about
  one session in three. If ~10,2 s ever returns, the first suspect is a task in the measuring
  path created with plain `xTaskCreate()` — not the camera. Signature: consumption halves
  exactly (5,77 → 2,88) on the master alone.
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
Session archive index: **[docs/data/README.md](docs/data/README.md)**.
Closed §1.x design notes: **[docs/PLAN_HISTORY.md](docs/PLAN_HISTORY.md)** (stub → `git show 1e62bca:…`).
⚠ STATUS / data / PLAN_HISTORY are history, not rules. Everything above this line is the rule set.
