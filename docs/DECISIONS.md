# elotto — decision log

CLAUDE.md holds the RULES. This file holds the EVIDENCE behind them: what was measured, what was
tried and rejected, and what it cost. Split out on 2026-08-19 because CLAUDE.md is loaded into
context every session and had grown to 830 lines, more than half of it history.

Entries are append-only and cited from CLAUDE.md as `[D<n>]`. A rule and its entry must be changed
together — a rule whose evidence has moved on is worse than no rule.

⚠ Everything here is dated. A measurement describes the instrument that made it; see D1.

---

## Instrument generations

### D1 — What splits the archive, and when
Sessions from different generations must never be pooled. The boundaries so far:

| date | what changed | effect on the data |
|---|---|---|
| 2026-07-29 | hardware change | nothing before it may be pooled with anything after |
| 2026-08-13 | block centring introduced | raw vs centred z; recompute both the same way or do not pool |
| 2026-08-18 | extraction speed-up 3,42 → 5,71 Mbit/s | same nominal `?run=`, 1,85× the bits per item |
| 2026-08-19 | onset flush on every window | the bit-to-item mapping changed |

Also: attended (`focus=1`) and unattended sessions are separate arms and are never pooled. v3 and
any v2.x session are separate instruments outright.

### D2 — The window you ask for is not the wall time you get
`?run=` sets a SEGMENT COUNT, and the wall time follows from the slowest node's bit rate.

- 2026-08-18, `run=5`/`gap=2`: window 5,05 s, gap 5,52 s, cycle ~10,6 s, 130.435 segments.
- 2026-08-19 early, same request: `focus_win_ms` 8721 for a requested 5000, `focus_gap_ms` 2081 for
  a requested 2000, cycle ~10,8 s. Same cycle, different split — at that moment the master was the
  slowest node (3,48 Mbit/s against the slaves' 3,95) where it used to finish first and wait ~3,5 s.
  The same seconds, counted as gap before and as window after.
- 2026-08-19 later, `run=2`/`gap=0,8`: the ordering INVERTED. Master 5,72 Mbit/s (`ms_extract`
  39,8 ms/pair — the idle rate, i.e. its extraction is not being preempted) against all three slaves
  at 3,66–3,68 (72,5 ms). A run needs 10,4 Mbit: master done in ~1,8 s, slaves in ~2,9 s. The master
  waits ~1 s per run and the wait lands in `focus_gap_ms` (2356 measured against 800 requested).

Two consequences that outlive the numbers: the run's wall time is set by the SLOWEST node (which is
why the round model takes `min(cam_mbit)`, D14), and **the nodes do not integrate the same
interval** — the master's window was a ~64 % prefix of the slaves'.

⛔ Chasing the split down is dropped by decision (user, 2026-08-19, for time). The statistics do not
depend on it: the segment count is fixed and z is normalised by its square root. What it sets is the
Focus protocol's hold time, and `RUN_SEGS_REF`/`RUN_MS_REF` are deliberately NOT re-calibrated to it.

⚠ Never measure it with tight polling. `/status` in a loop loads the master over HTTP and the window
is the max over nodes, so the observer changes the observation: 10,3 s while polling, 8,7 s
undisturbed.

---

## Session shape

### D3 — Unlimited-mode pool sizing maximises COMBINATIONS, nothing else
The chance a round's pool contains the real draw is
`C(p,5)/C(50,5) · C(q,2)/C(12,2)` = `[C(p,5)·C(q,2)] / 139838160` = **(combinations measured) /
(combinations that exist)** — the main/bonus split cancels completely. A pool rule is therefore
neutral exactly when it spends the budget and harmful exactly in proportion to the runs it leaves
unspent, and it costs twice, because a short round reaches its next 62-run scoring pass sooner.

A weighted rule was tried and **withdrawn the same day**: maximising universe coverage `p/50 + q/12`
weights a bonus number ~4× (there being only 12), pinned q at 5, and measured **210 of a 500-run
budget where 462 were available — 2,2× less coverage**. The bonus preference survives only as the
TIE-BREAK, where P is identical and it is free.

Verified 2026-08-18: Eurojackpot at `maxruns=50` sizes to 6+4 = 36 (the withdrawn rule answers
5+5 = 10). 6-of-49 has no split and was never affected.

### D4 — A truncated round used to draw the lexicographically first ids
Fixed 2026-08-19. The permutation was filled with `0 … round_total-1` and shuffled among themselves,
which randomises the ORDER but not the SELECTION: a truncated round measured the FIRST ids — in
6-of-49 the combinations built from the pool's lowest numbers, and in Eurojackpot
(`mi = i % main_combos`, `ei = i / main_combos`) a truncation below `main_combos` pinned `ei` to the
first euro pair for the whole round.

Now a forward partial Fisher–Yates over the whole space, which is an ordinary full shuffle when
nothing is truncated. Verified by simulation: 462-space, 146 drawn, χ² 298 at df 461 (was: ids 0–145
always, 146–461 never). Only the LAST round truncates — the `results[]`-full stop — which is why the
bug survived: that path had never run.

### D5 — Phase 0 scores each number with ONE long run, never repeats in place
The phase has been through four shapes; two were rejected on the same ground: repeated short reps
froze the panel and only the first window had an onset. One long run gives the same arithmetic — a
3× run is Σdev/√(3N), i.e. the Stouffer combination of three 1× runs — while keeping **one onset per
number**, which is the payload.

⚠ Under H₀ the combined z is already N(0,1): its SE is 1,0, not 1/√k. Ranking 50 unit-noise draws is
very noisy, and that is a real cost. It changes only WHICH numbers enter the pool, never the Phase-2
statistics measured on them.

### D6 — The observer gate is armed on `focus_mode`, not on `confirm`
2026-08-13. The UI sends `confirm=1` unconditionally and the gate has no timeout, so an unattended
5005-item run parked behind a Start button nobody was there to press: that pass ran **37,9 h wall
against 12,2 h of measuring**, ~25,7 h stalled at a gate. No observer, no observer gate.

### D7 — `LINK_MEAS_MS` had to scale with the run
A flat reply timeout was headroom for a short run and a DEADLINE for a long one: every slave would
still be measuring when it expired, all would look silent, and after `NODE_MISS_LIMIT` they would be
DROPPED — leaving a solo session that still looked healthy. `LINK_MEAS_MS_FOR(nseg)` is deliberately
generous: a late drop costs nothing, a false drop costs an arm measured at √(k−1) unnoticed.

---

## Ranking and centring

### D8 — Why ranking runs on `z_ctr` and not on the raw z
The 2026-08-13 full pass had per-node σ ≈ 1,0 *inside* every block while the block offsets jumped —
master to −6,33, slave1 to +24,13 as block means. The noise was fine; the zero point moved.
Re-centred, that pass is a clean null: max |z| **3,92** against the **4,13** expected as the maximum
of 5005 normal draws.

Costs, both accepted as pre-registration: centring removes any real effect CONSTANT across a whole
block (what remains visible is an effect varying BETWEEN items inside a block), and it shrinks σ by
(1 − 1/n_block), ~0,5 % at ~100 items per block — so σ(z_ctr) sits slightly under 1 by construction.

### D9 — Nearest-zero means nearest the PASS MEAN
Raw z carries the array's common offset — the 2026-08-05 pass ran at mean −1,82 — so |z_raw| ≈ 0
would select items about +1,8σ ABOVE the array's own centre: the opposite of neutral.
`results_near_mean()` picks by |z − mean|, which orders identically to the studentized |z − mean|/σ.

### D10 — The studentized-view checkbox of PLAN.md §2.3 was never built
The studentized value exists only as the `Z*` column. Do not cite it as a feature.

---

## Node health

### D11 — Soft-down trips on σ alone; |mean| only reports
The |mean| trip wire (`NODE_MEAN_SOFT` 1,50) was removed 2026-08-19 after a live session ran 50 % of
its items at k < 4 — the last 3,4 h at k = 2 — with `null_flags` 0, `fault` empty, `ok` true and
`nodes_ok` 4. Two of four arms out of the combine, and nothing in the published state said so.

Three reasons, in order:

1. **The offsets it fired on are made by the calibration sweep.** Per-block offset against the rung
   chosen for that block:

   | exposure | 4 | 8 | 16 | 32 | 64 | 128 |
   |---|---|---|---|---|---|---|
   | mean offset | −1,86 | −0,78 | −0,05 | +0,09 | −0,07 | −0,04 |

   exp ≤ 8 averages −1,11 over 10 node-blocks against −0,03 over 106, **t = −4,0**. Five of the
   master's six excursions sat on exposure 4 or 8. The dark gates (D18) remove those rungs at source.
2. **Centring already removes it.** `center_block()` subtracts each node's own block mean and
   everything ranked reads `z_ctr`, so a constant offset inside a block is gone from every number a
   result comes from. Keeping a +0,5-offset arm costs ~0; dropping it costs 13 %.
3. **It was not run-length invariant.** 1,50 in z is a bias of 2,3e-4 at `run=2` and 1,5e-4 at
   `run=5`, so the same camera tripped or passed depending on `?run=`.

**2026-08-26 — the offset survives on a GATED rung, and `mflag` has now fired on hardware.**
Measured on the 7354-item / 35-block session in `docs/data/2026-08-26_aborted7354_rescue/`, in which
the master reported `cam_cal=1` in **every** block, i.e. it was on a certified rung throughout:

| node | block mean | block σ | mflag | trip |
|---|---|---|---|---|
| **master** | **−0,4648** (min −4,141, max +0,056) | 0,9951 | **3** | 0 |
| slave2 | −0,0232 | 1,0124 | 0 | 0 |
| slave0 | −0,0372 | 0,9933 | 0 | 0 |
| slave1 | +0,0813 | 0,9908 | 0 | 0 |

So the answer to the open question is **yes**: gating the dark end shrank the offsets but did not
abolish them, and three blocks still crossed `NODE_MEAN_REPORT`. It remains a pure LOCATION effect —
master σ 0,9951 with no trip and no soft-down — which is exactly the split this decision rests on, and
centring removes it. **Reporting it and not tripping on it was the right call.**

As a cumulative Stouffer over `z_raw` the master's offset is **−39,8** over 7354 items against
−2,0 / −3,2 / +7,0 for the three slaves, and the combined raw figure is −19,01. ⚠ That number is the
instrument, not a result: on `z_ctr` the same statistic is **−0,0000** (max 5,5e-5 across all 36
blocks), identically zero by construction because centring subtracts the block mean. Anyone reading a
cumulative Z off the raw channel is measuring the master's exposure rung.
⚠ `drift_t` cannot see this: it regresses the block mean on the block INDEX, and a constant has slope
zero — this session read `drift_t` 0,85 while the master sat at −0,4645.

⚠ **2026-08-11 is not a counter-example.** That pass (master mean −7, slave1 +4,6, σ ≈ 1, all kept)
is why the σ-only rule was WRONG then and right now: it had no centring. Do not cite it against D11.

Replayed over the same 29 blocks: **24 of 29 blocks at k = 4 instead of 12, none below k = 3**, and
slave2's genuine σ trip still fires and still costs it 5 blocks.

### D12 — The clear bar is peer-referenced because a constant was the array's own median
It was fixed at σ ≤ 1,05. Measured over a 9 h four-node session, the per-block σ of a HEALTHY arm
here is 1,02–1,05 — so 1,05 was not a health threshold, it was the median. Meeting it four times
running is ~6–15 %, so a node that tripped once stayed down for the rest of the session whatever its
condition: slave1 tripped on ONE block (σ 1,290) 40 min in and was still excluded 5,9 h later with a
running z of +0,0025. 90 % of that session combined over √3, in silence.

⚠ **Replay a threshold change before believing it.** The first attempt kept 1,05 as the FLOOR and
changed nothing at all, because a floor pins the bar exactly at the value that was too tight. At
floor 1,10 slave1 clears after 11 blocks; at 1,10 with K 1,15 after 5; no other node is ever put down
by either.

There is no |mean| clear bar either, and that is not tidying: a criterion that cannot trip a node
must not be able to keep it down. The pair that stood there (floor 0,50, K 2,00) was the same defect
a second time — the peer median |mean| runs 0,02–0,30, so the floor bound in nearly every block, and
slave2 broke its streak three times on |mean| 0,52 / 0,65 / 0,74 while its σ never left 0,93–1,06.

### D13 — `NODE_SOFT_MIN_COMBINE` is 1, not 3
2026-08-13, user decision: a bad arm costs more than a small k. At four nodes a floor of 3 allowed
exactly ONE exclusion, so when two arms misbehaved the second stayed in. The 08-13 pass is the proof:
slave1 was excluded, the master (block means to −6,33) was kept, and block 4 published
(−6,33 + 0,27 + 0,22)/√3 = −3,38 with the master's offset intact.

### D14 — Quarantine fires on every trip, but only if the node was in the combine
Both simpler rules are wrong and both were measured:
- *only the first trip* waved through every later bad block (08-13: blocks 4, 14, 16 ranked);
- *every trip unconditionally* excluded 33 of 33 items per block, three blocks running, pass σ 0,000
  — nothing left to rank at all.

`center_block()` sets the contribution mask on the pass it already makes, so the test is free.

### D15 — slave1's ~4 % σ excess stayed with the BOARD, not the camera
Closed 2026-08-19. Mean per-block σ over 29 blocks of a 9 h loaded session: master 0,9990 · slave1
**1,0402** · slave0 1,0101 · slave2 **0,9964**. slave2 carries slave1's old camera since the
2026-08-17 swap and has the LOWEST σ of the four; slave1 keeps the excess with the sensor gone
(≈3,9σ on the difference).

Tolerating it is correct: a 1,04 arm inflates the COMBINED σ by 1 %, dropping it costs √3 against
√4 = 13 %. ⚠ slave1's long exclusions in past sessions were largely D12, not its condition — "was
soft-down for hours" is not evidence of a fault.

---

## Calibration

### D16 — Rung selection: lowest |bias−0,5| among candidates clearing σ with MARGIN
Selection used to take the FASTEST passing candidate, on the assumption that a shorter exposure means
a faster frame rate means more bits. That assumption is dead: the bit rate is CPU-bound, and a full
sweep measures 3,217–3,293 Mbit/s across exposure 4..512 — a 2,4 % spread, i.e. noise. The tie-break
was comparing eight identical numbers and picking whichever measured highest. Cost, measured: the
master picked exposure 16 (bias −3,7e-4) over 32 (−2,1e-4) on a 0,7 % rate difference, and across one
5-loop session its choice wandered 128 → 256 → 8 → 128 → 128, once landing on a rung a standalone
sweep had FAILED at −1,21e-3.

The σ margin (2026-07-27) exists because bias alone is not the property that hurts. Over a 200-loop
session, node .145 sat on exposure 32 in 91 of 127 logged loops and produced every σ excursion there
(per-loop σ SD 0,245, max 3,008), while on exposure 16 it was indistinguishable from healthy (SD
0,082 against 0,089 expected from sampling). Its exposure-32 rung sits ON the σ gate: it passes some
sweeps and fails others, and when it passed its bias often measured best — so the bias-only rule
selected it. A rung that scrapes a gate is a rung on a cliff.

⚠ This narrows the window in which a cliff-edge rung can be picked; it does not abolish it.

### D17 — No fold trial: a 3 s window cannot certify fold-off
Withdrawn 2026-07-26 after the trial worked exactly as designed. Fold-off measured a bias of 0,500845
— inside the 1e-3 gate — and won the rate tie-break at 5,57 vs 3,37 Mbit/s. The master then ran a
whole loop on it and its per-run σ went 1,043 → 2,153, taking the combined σ from 1,041 to 1,382. The
three slaves, still folded, stayed at 0,97–0,99.

The cause is that the unfolded LSB bias is NON-STATIONARY: within that same loop it moved from
+8,4e-4 at calibration to −1,3e-3 during the baseline minutes later — 2,1e-3 of travel against a
1e-3 gate. Neither gate could see it. The bias gate reads one window; the σ gate reads 3200-bit
mini-runs INSIDE that window, far too short to show drift over seconds.

**Re-tested 2026-08-26 and confirmed, on a much better-instrumented array.** `CONFIG_ELOTTO_CAM_XOR_FOLD=n`
OTA'd to slave0 alone; master + slave2 + slave1 stayed folded as the within-session control.
6-of-49, `run=1,5 s`, `calint=4 min`, 346 items, 4 blocks. Three independent failures:

1. **The sweep certifies NOTHING.** `ok:false, chosen:-1` — all 9 rungs fail `CAM_CAL_FAIL_BIAS`
   **and** `CAM_CAL_FAIL_SIGMA`. Folded slave1 certified 4 of 9 in the same minute. At the same
   rung 128: bias 0,493375 / σ 1,7233 against 0,499942 / 1,0080.
2. **Soft-down trips in block 3.** Per-block z σ 1,176 / 1,033 / **1,273** / 1,058 against
   0,886–1,101 across the three folded nodes; the block was quarantined and `k` fell to 3 for
   126 of 346 items. Milder than 2026-07-26's 2,153 — shorter run and a better-lit rung — but
   it still trips `NODE_SIGMA_SOFT` inside three blocks.
3. **The entropy channel, which did not exist in 2026-07-26, is the sharpest detector by far.**
   Raw z_h over 346 items: master +0,079 · slave2 +0,054 · slave1 −0,138 · **slave0 −201,7452**
   (sd 11,0). Two hundred σ off the null. The unfolded LSB stream carries gross per-frame
   spectral structure that the XOR fold cancels; `ENT_Z_CLAMP` fired on it continuously.

⚠ **The failure is SCALE, not location.** The fold-off block MEANS were −0,61 / −1,07 / −0,13 /
+0,60, i.e. near zero — the damage is entirely in σ. Any correction that only recentres the
stream therefore cannot rescue fold-off; see the adaptive-bias entry in the dropped list.

⚠ **Open, not chased down.** `cam_bias` 0,493490 converts through the D19 relation to −36 z per
run at `run_segs` 39130, but the node's measured block mean was −0,61. `gcp.c` reads the same
ring words the bias statistic counts, so the two should agree and they differ by ~50×. The gate
verdict is still right; the reason is not fully understood.

Data: `docs/data/2026-08-26_foldoff_trial_slave0/`.

### D18 — The dark end of the ladder is gated (2026-08-19)
`CAL_MIN_MEAN_PX` 5,0 and `CAL_MAX_ZERO_DIFF` 0,125. The premise is that photons do the whitening,
and the bottom rungs have none: at exposure 4 the frame sits at `mean_px` 3,1–3,5 with **13,6–16,7 %
of pixel differences exactly ZERO** against 6,7–7,1 % at 128, and a zero difference has a
deterministic LSB. Those rungs are not a dimmer version of the same source, they are a partly frozen
one — and they are what the |mean| soft-down trip was firing on (D11).

Both gates are cut below the lowest rung that behaves (16: `mean_px` 5,3–5,6, `zero_diff` 0,108–0,120)
and above the highest that does not (8: 3,8–4,1 and 0,123–0,151). They are **deliberately
redundant** — `mean_px` is the physical quantity, `zero_diff` the mechanism — and a rung must clear
both: on the master's own sweep exposure 8 passes `zero_diff` at 0,1227 and is caught by `mean_px`
alone.

Verified against two live measured sweeps: exposures 4 and 8 rejected on both nodes, 16…128
unchanged, no node left without a candidate. ⚠ If the lamp is dimmed these start rejecting rungs that
used to pass. That is correct; the answer is light, not a lower floor.

### D19 — The bias gate is scaled by the run length, and is noise-limited
A bias `b` over `nseg` segments is a per-run z offset of `(b−0,5)·200·√nseg / GCP_SEGMENT_SD`. The old
fixed 1e-3 therefore admitted **6,5 z at `run=2` and 10,2 at `run=5`** against a node-health bar of
1,5: the sweep was certifying exactly the rungs the health gate then tripped on, and the same physical
camera passed or failed depending on `?run=`.

The bar is now a z offset (`CAL_MAX_Z_OFFSET` 1,0) converted with the session's segment count via
`gcp_z_per_bias()` — which is why the count travels on `K`. Never tighter than `CAL_BIAS_SE_K` × the
window's own SE(bias), never looser than the old 1e-3, so it can only tighten.

⚠ **It cannot resolve the bar it feeds.** 4,8 Mbit per rung is SE(bias) 2,3e-4 = ±1,5 z at `run=2`,
so the effective bar lands at ~6,9e-4. That is a property of the 10 s budget; the fix is more bits per
rung, not a smaller number. Each rung's implied `z_off` and the bar that bound it are logged.

### D20 — `CAL_MAX_MEAN_PX` is 100,0 and is only a backstop
It was a light-LEAK floor at 64,0, written when the cameras were meant to sit dark. The enclosure is
now deliberately lit, so a high `mean_px` means the lamp is on. At 64,0 it rejected the best rung the
rig has produced: exposure 64 at `mean_px` 68,7 gave bias −4,8e-5 with σ 0,998 and autocorr 0,0009 —
no quality failure at all, refused on light level by 7 %. The value sits inside a measured safe zone:
68,7 is clean, 118,6 is decisively broken (σ 1,91, autocorr 0,0097, bias −8,1e-3).

Across every lit sweep measured, exposures 128/256/512 fail BIAS and SIGMA on their own; saturation
announces itself. Those are the real protection.

### D21 — The sweep budget is 10 s, and each rung is scored on ~1,1 s
5 s was too short — a long pass left all nodes with `cam_cal=0`. At 10 s over 9 candidates, per-rung
bias/σ are correspondingly noisy and rungs near a gate boundary flip between sweeps; that is the
failure D16's σ margin exists to contain. Per-loop calibration is **statistically neutral**
(A−B = 0,52 SE over 6×430 runs per arm).

---

## Extraction

### D22 — 3,42 → 5,71 Mbit/s per node (2026-08-18), bit-for-bit the same stream
Three changes, measured separately because the first guess was wrong twice:

| step | Mbit/s | note |
|---|---|---|
| baseline (`-Og`) | 3,42 | 52,8 CPU cycles per pixel |
| `-O2` (`COMPILER_OPTIMIZATION_PERF`, both projects) | 3,81 | ×1,11 — the compiler was not the problem |
| inline popcount | 4,60 | ×1,21 |
| word-wise extraction | **5,71** | ×1,24 |

- These cores have **no Zbb**: `__builtin_popcount` compiles to a CALL to `__popcountsi2` (confirmed
  with objdump). `process_word()` ran it five times per 32 pixels — one for the bias, four for the
  autocorrelation gate. The inline SWAR replacement beat `-O2` on its own.
- `LSB(b − a) ≡ LSB(a) ⊕ LSB(b)` — bit 0 of a subtraction never depends on a borrow, so the
  per-pixel subtraction is unnecessary and 4 pixels are one XOR of two 32-bit loads. ⚠ The usual
  `haszero` SWAR trick COUNTS WRONG (a zero byte borrows into the next and flags it too); `zero_diff`
  uses the borrow-free form. The self-test caught the fold bug that shipped with the first draft.

### D23 — The idle ceiling is the sensor, and we are at 98,5 % of it
The old "71 % of the ceiling" figure assumed 50 fps from the datasheet. `fps_raw`
(`camera_fps_probe()`, run by `/camtest` before the benchmark so it sees an idle CPU) times the
sensor with extraction stopped: **36,11 and 36,22 fps** on two runs → a pair every 55,2–55,4 ms
against a live 56,0 ms, i.e. 18,1 pairs/s × 320.000 bit = **5,80 Mbit/s** against 5,71 measured.

The measured PSRAM read floor is 7,1–7,9 cycles/pixel against ~21 for extraction+statistics.

### D24 — Why the last two extraction changes bought nothing (answered 2026-08-18)
`mean_pixel` folded into the diff loop and the `vTaskDelay(1)` thinned to every 4th pair were
predicted at ~13 % + ~9 % and measured **0,0 %** (5,707 → 5,707 Mbit/s). Both did exactly what was
predicted TO THE CPU. The rate did not move because at idle the loop is not compute-bound: it spends
its spare time blocked in `VIDIOC_DQBUF` waiting for frames, and a CPU saving is absorbed there.

The suspicion that the 256 KB benchmark harness was misreporting extraction cost was WRONG. Repaired
to the real 2×640000 B geometry it says 37–41 ms per pair against a live `ms_extract` of 39,5 — they
agreed all along. The faulty step was the inference: the ~14 ms was assumed to be extraction overhead
the benchmark had missed, and it is DQBUF wait.

Two candidate walls fit `ms_wait` equally well — a genuinely slower sensor, or a fast sensor whose
frames cannot be written while the CPU hammers PSRAM. Both were tested:
- **`CAM_BUF_COUNT` 4 → 8**: `ms_pair` stayed 56,0, only the split moved (wait 15,1 → 11,9,
  extraction 39,1 → 42,2). Reverted; it costs 2,5 MB of PSRAM and buys nothing. ⛔ Do not re-propose.
- **`fps_raw`**: D23. We are within 1,4 % of the sensor's own cadence.

### D25 — Under LOAD the loop is compute-bound, the other way round
Measured 2026-08-18 during a loaded 6-of-49 run, all four nodes:

| | idle | loaded |
|---|---|---|
| `ms_pair` | 56,0 | **85,4** |
| `ms_wait` | 14,6 | 13,0 |
| `ms_extract` | 39,5 | **69,8** |
| `mbit_s` | 5,71 | **3,76** |

The GCP consumer runs above `ELOTTO_CAM_TASK_PRIO`, so extraction is preempted and a pair costs 70 ms
of wall time instead of 39,5. The sensor still offers one every 55,4 ms; the loop takes 85 and drops
frames. This is the mechanism behind "the camera rate collapses under duty cycle" — it is preemption,
not the duty cycle as such.

⚠ **Do not read D23 as "the extraction path is finished".** It is finished for the IDLE rate and only
that. A further speed-up would still pay during a session. Two conditions before anyone tries: prove
it with `ms_extract` under LOAD, never at idle; and note that raising the loaded rate pushes a 5 s
window toward 182.000 segments against `EL_SEG_MAX` 200000, splitting the archive again (D1).

### D26 — The soft-float calls per segment stay, except the one that was exact
2026-08-19. The P4 FPU is single-precision, so `(ones - 100.0) / 7.07106781` compiled to `__floatsidf`
+ `__subdf3` + `__divdf3` + `__adddf3`, together costing more than the popcounts did. Both obvious
cures — multiplying by the reciprocal, or summing `ones` and dividing once — are arithmetically equal
and **not bit-identical**, so either would shift the last bits of every z in the archive.

**`__subdf3` was the exception and is gone (2026-08-21).** `ones` is in [0,200], so `ones - 100` is an
integer subtract whose result converts to double exactly — and `(double)ones - 100.0` is exact for the
same reason, which makes the two **bit-identical, not merely equivalent**. Verified by enumeration
over all 201 values of `ones`. It was held back from the popcount change deliberately, so that change
could be measured alone. ⚠ Bit-identical means this does **not** split the pooling table: it is the
only rearrangement in the z path that does not.

The popcount call, by contrast, was removed: seven `__popcountsi2` calls per segment, ~913.000 per run
at `run=5`. **Measured +7,6 % bit rate under load** (3,734 → 4,019 Mbit/s, `ms_extract` 70,8 → 64,9,
three slaves in close agreement) against an estimate of ~1 % — the fourth prediction in two days wrong
the same way: a library call costs far more wall time under preemption than its cycle count suggests.

### D27 — Verification trap: a node serves the OLD image while the OTA writes
Hit twice. `until curl http://node/` succeeds immediately, so two measurements were taken from the
previous binary before this was noticed. Poll `fw_sha` in `/status` until it CHANGES.

---

## Illumination and power

### D28 — The enclosure is LIT, and sealing it made things worse
"Controlled light" was always the goal; it is not the same as darkness. Sealing it cost raw LSB
uniformity and σ — photon shot noise was doing real whitening work.

### D29 — Never power illumination from a node's VSYS pin
An LED on VSYS with PWM dimming produced bias −4,33e-3 and certified **0 of 9** rungs. The mechanism
is **conducted, not optical** — PWM current on the rail feeding the sensor's analog supply. It was
misdiagnosed as optical flicker first; the giveaway is that a separate supply restored 8 of 9 rungs
*while `mean_px` barely changed*.

### D30 — Let the illumination settle ~30 min after physical work
2026-08-17, measured: after the camera swap slave1 read `mean_px` 6,69 at exp 128 during the sweep and
27,0 settled — a 4× rise that then held to ±0,4 % over 18 min. A sweep run during that rise picks a
rung that no longer applies, which is exactly what moves block offsets. The same day, a test session
started right after the swap put two nodes soft-down while a repeat twenty minutes later put none.

⚠ Do not judge the light by one `mean_px` reading — sweep, or measure a time series. A single number
cannot separate a dim lamp from a short exposure, and readings taken during a sweep are not comparable
with settled `/diag` values.

### D31 — All four nodes on PoE, permanently
User decision. The master's separate USB supply was the Risk-1 control and ⛔ will not be restored.
The consequence, stated once: inter-node correlation can no longer be attributed to the power rail
versus anything else. The one measurement that could separate them is already in the bank — an arm run
while the split was intact found master↔slave pairs at +0,023 against slave↔slave +0,024, largest
single pair on the *isolated* node — and cannot be repeated on this rig.

### D32 — Do not switch camera hardware on the theory that the OV5647 is the problem
The capture runs at RAW8 800×800, ~13 % of the sensor, because the pipeline is PSRAM-bound at a 640 KB
diff per frame pair; more megapixels would LOWER the bit rate. If it is ever tried, buy ONE and run a
matched pair, not four.

---

## Focus protocol

### D33 — The panel lights ~70 ms before the bits start, and that is accepted
`focus_publish()` runs, then the trigger goes out, then the ring flush waits for a fresh frame pair —
so the lead is about one frame pair: ~56 ms idle, ~85 ms under load (derived from `ms_pair`, not
directly instrumented). The older wording "for exactly the window its bits are collected in" was never
what the code did, and it is not what the protocol wants: an observer needs to perceive the target
before sampling starts.

⛔ SETTLED (user, 2026-08-19). It was raised as an open pre-registration question — visual perception
plus attentional engagement is usually put at 100–300 ms, so the lead is arguably too SHORT, and
`READY_SETTLE_MS` holds a full 1 s at the observer gate for the same reason. Judged good enough. Do
not re-open it, and do not "resolve" the asymmetry by moving either number.

### D34 — The onset flush had two defects, both fixed 2026-08-19
Nothing consumes the ring during the gap, so it is FULL when a window opens: 524288 bits captured
before the item existed — 10 % of a `run=1` item, 2 % of a `run=5` one. Not bad bits, MISLABELLED ones.

- It was gated on `focus_mode`, so **the matched no-focus control was not matched**: the deferred
  attended-vs-unattended comparison would have measured the settle as much as the observer.
- It called `camera_stats_reset(1)`, which also zeroes the camera accumulators, so in an ATTENDED
  session every block's `cam_bias`/`cam_mbit` in `/loops` described only that block's last item.
  `camera_ring_flush()` is the statistics-preserving primitive; `camera_stats_reset()` stays for
  calibration, where zeroing is the point.

⚠ The trigger is sent BEFORE the master settles, so all four flush in parallel; the other order would
offset the master's window from the slaves' by ~85 ms.

### D35 — A flush that does not finish VOIDS the run
2026-08-19, and not a cosmetic guard: the ring is dropped at a PAIR BOUNDARY, so if no pair arrives
the ring is never dropped at all and the run would consume exactly the stale bits the flush exists to
remove — the one case where the safeguard turns into a disguise.

It is deliberately NOT escalated to a camera fault: a pair costs 56–85 ms against the 500 ms cap, well
short of `CAM_STALL_TIMEOUT_MS`, and a transient must not cost an arm for the rest of the session.
Counted in `flush_timeouts` because a safeguard that fires must not be indistinguishable from one that
never had to.

### D36 — Baseline runs flush too
Not consistency for its own sake: `LoopStat.base` is the INDEPENDENT cross-check against the block's
own master mean (`raw_m`), and two estimates of one offset are only comparable if their bits have the
same provenance. That cross-check is what showed the 08-19 block-3 excursion was already present
before the block began (base −1,802 vs block mean −1,712).

---

## Architecture

### D37 — The z primitive is shared so the nodes cannot disagree
The combine is Σz/√k over nodes, which is only meaningful if every node computes z identically. A
divergence would have been invisible: a wrong-but-plausible z looks exactly like a result. Until
`components/elotto_gcp` existed the function was duplicated in `main/sensor.c` and
`elotto_slave/main/slave.c`, arithmetic identical but maintained twice.

⚠ `GCP_SEGMENT_SD` is the literal `7.07106781`, NOT `sqrt(50.0)` — every z the rig has recorded came
from that constant, and a change in the numbers must never be a side effect of tidying.

### D38 — The segment count travels on the wire, and a receiver does not clamp it
`seg_from_cmd()` falls back to the slave's own `CAM_SEGMENTS` (8000) and logs it to a serial port
nobody watches. Master at 260869 segments and slaves at 8000 would be combined as if they had
integrated the same window, and **no gate would fire**: each z is normalised by its own √segments and
stays N(0,1). Raising `EL_SEG_MAX` therefore means fixing that fallback to REJECT, in both firmwares,
together. A `_Static_assert` in `sensor.h` re-checks `RUN_S_MAX` against the calibration at every
build — verified to fire by temporarily setting `RUN_S_MAX` back to 15.

### D39 — The round-length model is bits over rate
The old `CYCLE_RUN_PCT` (`1,36 · run_ms + gap_ms`) measured 4,43 s at `run=2`/`gap=0,8` where it
predicted 3,52 — 21 % short — and was not merely stale: a percentage of the REQUESTED window is the
wrong shape. What a run costs is the time the slowest node needs to make its bits, so the bit count
and the rate must appear separately or a change to either invalidates the constant silently.

`CYCLE_FIXED_MS` (780 ms: flush, trigger, collect, publish) was solved from that same point: 4433
measured − 800 requested gap − 2851 ms of bits at 3,66 Mbit/s.

The new form also fits the OTHER instrument generation, which the old one could not: at `run=5`/
`gap=2` and the ~3,48 Mbit/s of the pre-08-18 nodes it gives 10,3 s against a measured ~10,6–10,8.

### D40 — A webserver that never comes up is a rolled-back image
`webserver_task` waits 30 s for an IP and then deletes itself without retrying, so a slow DHCP lease
leaves a node that pings but refuses port 80 — and the image is rolled back on the next boot. This
took all four nodes down once (2026-08-13) and looked like four simultaneous firmware failures.

### D41 — The archive keeps every node's z, so exclusions are reversible offline
`s_node_z` (`NUM_RUNS × MAX_NODES` floats, ~128 KB in PSRAM) and the `z0..z3` columns hold each arm's
raw z for every item, INCLUDING arms that were soft-down at the time. A soft-down decision therefore
costs live sensitivity but destroys nothing: any session can be recombined afterwards under a
different rule. This is what made D11 provable after the fact.

### D42 — A full results[] compacts at the round boundary, it does not end the session
The 2026-08-19 unlimited session stopped after 11,6 h with "results buffer full (8000 items)". It did
not have to: at a round boundary every block of the round has closed, so `center_block()` has
replaced every provisional `z_ctr` and the ranking key is FINAL. From there the three published
tables are the only individual items anyone reads, and everything else the session reports — pass
mean, σ, χ², v_eff, the Bonferroni line — is a function of n, Σz and Σz².

- **Top-N and Bottom-N stay exact** at any K ≥ TOP_N, by construction: the maximum of a union is the
  maximum of the per-part maxima.
- **Nearest-zero is not exact by construction** — its target, the pass mean, keeps moving. In
  practice it barely moves: centring pins each closed block at mean ≈ 0, so the pass mean sat at
  ±0,00000 after every one of the 18 rounds, and the final nearest-5 ranked 1,1,1,2,1 within their
  own rounds. `PASS_KEEP_PER_TABLE` is 16 rather than 5 for what headroom does not cover: a round
  whose last block is still open, and a quarantine removing several survivors at once.

Replayed against those 8000 items, compacting at EVERY round boundary rather than only when needed:
**48 rows held instead of 8000**, n/void/excl identical, σ and χ² bit-identical, mean off by 7e-19,
and Top/Bottom/Nearest all 5/5.

⚠ **This is not "keep only the 15 published items".** That was measured too, mid-block, where
`z_ctr` is still provisional: a running top-5 on the raw z keeps **3 of the true 5**, and the item
that ends 4th sits at raw rank 16 when it is measured. The round boundary is what makes compaction
lossless, not the count kept.

⚠ **It runs only when the next round would not fit.** The dropped rows cannot be recovered, and the
per-node `z0..z3` of a dropped item is how `[D11]` was found — last resort, not policy.

**First run on hardware, 2026-08-20 — and it was wrong.** `round_base` was assigned from
`items_done` where it is an INDEX into `results[]` and had to come from `runs_completed`. The two
were identical until this decision made `pass_compact()` lower the one and deliberately keep the
other, so the round after a compaction wrote past the compacted array, left the dropped rows in
front of it, and counted them again on top of the moments they were already folded into. The
session reported `pass_n_valid` 15806 = 7806 + 8000 for 8019 items, with every survivor duplicated
in `top`/`low`/`near`. Wrong: everything scaling with n — `pass_stouffer`, `comparisons`, the
Bonferroni line.
⚠ **The tell is n, not σ.** Doubling identical values barely moves mean or σ, so the σ ≈ 2 symptom
this entry warns about below does not fire. Check `pass_n_valid` against `completed`.

Fixed the same day, and **verified on hardware** in the 08-20 Eurojackpot session: 48 rounds,
`compacted` 15828, `pass_n_valid` == `completed` == 17766.

⚠ The fix split the round's base in two, and the first version of it shipped with the display still
reading the wrong one — "item 16184 / 378" on the page and a nonsense `items=` in the CSV header.
**`round_base` is a results[] index; `round_item_base` is an items_done value.** Anything counting
ITEMS takes the second. They are equal until the first compaction, which is exactly why the mistake
survives testing on any session that does not fill the buffer.

Consequences elsewhere: `runs_completed` now means ROWS HELD and `items_done` is the session count
(`[D41]`'s archive is what shrinks, not the statistics); `LOOP_HIST` went 128 → 1024, because at 52
blocks per 11,6 h the old value was ~28 h and the first session able to outlive the buffer would
have gone blind there instead.

**Verified by replay, not yet on hardware**: exercising it needs a session that actually fills 8000
items. The cheapest path is 6-of-49 unlimited at `maxruns=5005`, where the second round boundary
cannot fit and compaction fires after ~3,7 h at `?run=1`.

---

### D43 — The spectral structure is DRIFT, not a row line; the pre-fold monitor (2026-08-26)
`GET /specdump?segs=<n>`, on every node: the mean Welch periodogram of a real window off that
node's own camera, 511 normalised bins. `/spectest` proves the FFT arithmetic; this says WHERE the
power sits, which is what H by construction cannot.

**The row-geometry prediction was refuted.** At RAW8 800x800 the fold-off row rate is 4,0 segments
per row and should have put a line on bin 256. Measured over four conditions, bin 256 reads 0,59x
to 0,94x of the mean — **below it, in every one**. The arithmetic was right, the premise was not:
there is no spatially periodic structure in the LSB stream for the row rate to alias.

| condition | sd/mean | bin 1 | bins 1-20 | bin 256 |
|---|---|---|---|---|
| slave0 fold=1 exp64  | 0,1754 | 1,07x | 4,06 % | 0,94x |
| slave0 fold=0 exp64  | 0,1752 | 1,14x | 4,27 % | 0,81x |
| slave0 fold=1 exp128 | 0,6904 | **8,87x** | 8,19 % | 0,59x |
| slave0 fold=0 exp128 | 1,1396 | **23,81x** | 9,15 % | 0,79x |
| flat, M=32 | 0,1768 | 1,00x | 3,91 % | 1,00x |

What there is instead is a monotone excess at the LOWEST bins. Bin 1 is a period of 1024 segments,
the longest the window resolves — **drift inside the window**, and the direct spectral confirmation
of D17's non-stationary unfolded bias.

⛔ **A stride or shuffle fold is therefore pointless, and so is spatial shuffling** — both attack
spatial structure and there is none. This closes the option raised the same day; it was never
built.
⛔ The MIPI/CSI clock was never a candidate and still is not: extraction reads a completed frame
out of PSRAM in raster order, not in step with the lane, so a timing clock cannot produce a fixed
segment period at all.

**EXPOSURE is the variable, not the fold.** At exp 64 both arms are flat; at exp 128 both are
disturbed. The fold SUPPRESSES the drift without removing it — at 128 it takes bin 1 from 23,81x to
8,87x and the emitted σ from 1,8359 to 1,4233, still over `NODE_SIGMA_SOFT`.
⚠ exp 128 is a rung **slave0's own sweep never selects** (it used 16/32/64 through the 7354-item
session); it was forced by hand with `POST /expose`. The run shows the gates being right, not a
fault. So the fold's job is correctly named at last: it squares a **drifting bias**, a TEMPORAL
defect — which is why squaring is the right minimal operation and why nothing spatial would help.

**The pre-fold monitor** — `raw_bias` / `raw_sigma` / `raw_sigma_n` in `/diag`, from `cam_raw_t` in
`extract.h`. Everything the rig published about the BIT stream was measured after the fold; the
pixel-domain values (`mean_px`, `zero_diff`) are before it; the raw LSB stream in between was
unmonitored, and that is where a degrading sensor shows first. It earned itself immediately: at
exp 128 slave0 reads raw_sigma **1,7680** against an emitted 1,4233, so the front end is degraded
and the fold is masking part of it — a number that previously required flashing a fold-off image.
⚠ With the fold OFF the raw and emitted pairs must agree exactly, and do (0,491186 / 1,8359 on both
sides); `case_equal()` holds ref against fast on the same struct, and cross-checks the count against
the emitted words at fold-off.
Cost: `/camtest` `ms_pair_ext` **39,3 ms** with the monitor live, against 39,5-39,8 historically —
no measurable cost, and `ns_raw` prices it directly against `ns_fast`.
⚠ Still to confirm under measurement LOAD on a slave, which is the only place D25 says a cost would
show.

**Cost, measured, and the duty cycle it forced.** `/camtest` prices the monitor directly:
`ns_raw` **68,5** against `ns_fast` **53,3** ns/pixel, i.e. **+28,6 %** on the extraction loop. At
idle that is invisible (`ms_pair_ext` unchanged, rate still 5,70-5,72) because the loop waits on the
sensor and the cost is absorbed in DQBUF — the same asymmetry the pixel-sum note in `extract.h`
documents. Under measurement load the loop is compute-bound (D25) and it would come straight off the
bit rate. So it runs on every **`CAM_RAW_EVERY` = 8**-th frame pair: ~3,6 % amortised, verified as a
clean 1:4 ratio of `raw_sigma_n` to `sigma_n` on hardware.
⚠ One frame is 640000 px = exactly 200 complete 3200-bit mini-runs, so skipping whole frames never
straddles a mini-run boundary. If the capture geometry changes that stops being true.
⚠ `ns_raw` in `/camtest` is the UNGATED cost and stays at +28,6 % — it prices the monitor, not the
duty cycle.

**The collector.** `GET /diagjson?all=1` fires `D` at every node and returns the whole array's
front-end health in one object — live exposure/gain, folded bias/σ, pre-fold bias/σ, rate, stalls,
soft_down, reboots, fw_sha, in discovery order with the IP on each row. `D` grew `,raw=` and `,exp=`,
both TAGGED and appended like `,fw=`, so a node too old to send them reads as absent rather than
misread. No new handler was spent: the cap is at 23 of 24.
⚠ 409 while measuring — `slaves_diag()` is only safe between loops.
⚠ Row 0 is the master and `slaves_diag()` cannot fill it; it is filled from the master's own camera
so the four rows are comparable. An unfilled row 0 read as a dead master.
⚠ `cal_fold`/`cal_exp` are what the last SWEEP chose; `exposure`/`gain` are LIVE. They differ after a
manual `POST /expose` or a sweep that certified nothing.

**⚠ OPEN, and the strongest candidate change: the sweep's tie-break is noise-limited and the
pre-fold bias is not.** Among candidates clearing every gate the sweep picks the lowest
|bias − 0,5| — on the FOLDED bias, which is the quantity the fold has already squared into the
sampling noise (D19 says the gate is bounded by its own sampling error). The first sweep to publish
both columns shows what that costs, on the master:

| exp | folded bias | folded σ | raw_bias | raw_σ | mean_px | verdict |
|---|---|---|---|---|---|---|
| 4   | 0,499520 | 1,0140 | **0,484580** | **1,2412** | 3,42 | fails DARK+ZDIFF |
| 16  | 0,500029 | 0,9914 | 0,492230 | 1,0666 | 5,59 | **CHOSEN** |
| 128 | 0,500050 | 0,9862 | **0,499163** | **0,9551** | 28,63 | passes |
| 256 | 0,498449 | 1,0619 | 0,487436 | 1,4832 | 53,47 | fails |

exp 16 won by **2e-5 of folded bias** over exp 128 — below the sweep's own resolution — while on the
pre-fold bias 128 is better by a factor of **8** (|Δ| 8,4e-4 against 7,8e-3) and sits at mean_px
28,63 against 5,59, barely over the `CAL_MIN_MEAN_PX` floor. D18's premise is that photons do the
whitening, so the brighter rung should win, and the folded criterion cannot see it.
⚠ Note also exp 4: folded bias 0,499520 looks FINE. The fold hides how bad the dark rung is, and it
is caught only by the dark gates. The raw column shows why those gates had to exist.
**Not changed.** It is one sweep, and moving rung selection moves every node's block offsets, which
splits the pooling table (D1). It needs replaying against a session's `/loops` with both columns
before it becomes the criterion — which is now possible, and was not before.

Data and the full write-up: `docs/data/2026-08-26_specdump/`.

### D45 — The pre-fold z: the channel the fold throws away (2026-08-26)
The XOR fold maps a raw bias ε to ~2ε², so at equal measuring time a MEAN-BIAS effect survives it
multiplied by **√2·ε**:

| ε | z_roh (1 s) | z_gefaltet | suppression |
|---|---|---|---|
| 1e-3 | 4,568 | 6,46e-3 | 707× |
| **1e-4** | **0,457** | **6,46e-5** | **7071×** |
| 1e-5 | 0,046 | 6,46e-7 | 70711× |

That is the quantity a GCP-style experiment exists to look for, and the fold was destroying it. The
defence of the fold up to now was about σ and stationarity and never about signal — an omission.

**The fold stays** — without it there is no stable null (D17: the unfolded bias wanders ~2,1e-3 over
minutes, twenty times a hypothesised 1e-4 effect) — but the raw stream is now ALSO scored, combined,
block-centred and archived, as a third channel beside z and z_h.

⚠ **RANKING AND ARCHIVE ONLY, never a p-value.** Raw σ runs 1,03–1,10 on certified rungs where the
folded stream is at 0,997–1,001. Same compromise as the entropy channel, same reason.
⚠ **It covers MORE bits than the folded z over the same window in TIME**: a segment pulls seven words
and uses only 8 bits of the seventh, while all seven were physically measured. Normalised as the
plain binomial `(ones − N/2)/(√N/2)` over the bits actually consumed, which is identical to
`Σ(ones−100)/(GCP_SEGMENT_SD·√(N/200))` and needs no fictitious segment count.
⚠ **Centring matters more here than anywhere.** Measured on hardware, per-node RAW pre-fold z on one
item: **−7,13 / +0,53 / −46,89 / −27,28**. Uncentred it ranks NODES, not items. After centring the
combined value was 1,1627.
⚠ **The provisional value is the worst of the three.** Before its block closes an item's `zp_ctr` is
uncentred, i.e. dominated by a 20–95σ node offset. A `wpre>0` live table is meaningless until the
first block closes. (Leaving it unset was a real defect for one build — the record is reused across
items and held a stale value.)

**Key.** `?wpre=<0..1>`, **default 0**, so the channel is measured and archived from the first session
without moving a ranking until asked for — added post-hoc, it is a third ticket in the same lottery
and needs per-session pre-registration exactly as `?went=` did.
`key = ((1−w−p)·z_ctr − w·z_h + p·z_pre) / √((1−w−p)² + w² + p²)`, three unit-variance independent
halves so the key stays unit-variance at every weighting. **z_pre enters with a PLUS** — it is a z on
the same scale and direction as z_ctr; only entropy is negated. `went + wpre > 1` answers **400**: the
z half would go negative. Clamped on the same `ENT_Z_CLAMP` bar; the archive keeps the real value.
Verified by hand from the CSV: z_ctr 1,2656 · z_h −0,5849 · z_pre 1,1627 at w=0,5 p=0,3 → **1,4509**,
the published value.

**Mechanics.** The raw ones count rides WITH each emitted word in a parallel `uint8` ring so the two
z values cover the same window — a free-running counter advances with the producer and includes bits
the consumer never saw (ring drops). ⚠ `CAM_RAW_EVERY` is **gone**: a diagnostic may be sampled, a
measurement channel may not.
⚠ **`NUM_RUNS` 8000 → 7200.** `results[]` is in INTERNAL RAM and the double in `RunResult` forces
8-byte alignment, so one more float cost 8 bytes per item — 64 KB — and the image stopped LINKING
(4913 bytes of discarded sections). The cap is the backstop for a compaction that cannot allocate
(D42), not the normal end of a session. ⚠ But Eurojackpot's 7920 no longer fits in one uncompacted
pass; a full Euro run now compacts once near the end. **Verify that, do not assume it.**

Wire: `Z:<z>,<H>,<z_pre>`, appended third; a node with no H sends a literal `nan` to hold the slot
open rather than shifting the field forward. CSV: `zp_ctr;p0..p3` appended last, `pre_w=` in the
header. ⚠ **`pre_w` splits the pooling table** for the tables, exactly as `ent_w` does.

⚠ Cost still UNMEASURED under load: the raw counter is now always-on and `/camtest` prices it at idle
only (`ms_pair_ext` 39,2, unchanged), where the loop waits on the sensor. Watch `ms_extract` and
`cam_mbit` on a SLAVE during a real session.

## Dropped and deferred

⛔ **Dropped by decision — do not re-propose:**
- the node-drop test, the camera-fault/reboot path, the camera-stall abort;
- restoring the master to USB power (D31);
- raising `CAM_BUF_COUNT` (D24 — measured, no effect);
- the second-core split for extraction (D23 — no headroom left at idle);
- reintroducing the on-chip TRNG in any form — **including an LFSR fed from the camera bits**
  (proposed 2026-08-26). An LFSR whose feedback is XORed with the raw stream IS the canonical
  whitened-hardware-RNG construction, so it falls under this line; it is named separately because the
  line did not read as covering it. Simulated over 8,2 Mbit per arm (32-bit polynomial 32/22/2/1):
  on a source carrying the measured fold-off bias plus a period-4-segment line it reaches
  bias 0,499929 / σ 0,9971 / autocorr 0,0004 — and the XOR fold reaches 0,496249 / 1,0015 / 0,0009,
  i.e. **the LFSR buys nothing the fold does not already deliver**.
  ⛔ What it costs is the instrument: fed a **frozen camera** (every diff zero, every LSB
  deterministically 0) the LFSR emits bias 0,499993, σ 1,0001, autocorr 0,0004 — **it passes every
  gate this project has**, where the raw and folded paths both read bias 0,000000 / σ 0,0000 and are
  caught instantly. The 2026-08-26 fold-off trial detected a merely *degraded* source in three
  independent channels; behind an LFSR all three would have read clean.
  Three further reasons: it has STATE, so it smears a time-localised deviation over the register
  length and beyond, where the fold is memoryless and local (`bit(i) = LSB(2i) ⊕ LSB(2i+1)`); the
  entropy channel's closed-form null assumes an i.i.d. source and does not hold for an LFSR sequence,
  so `z_h` would become meaningless; and it puts a PRNG in the z path, which `fast_rng()` is
  explicitly kept out of. The "cheap in hardware" argument does not transfer — the fold costs no
  compute at all, it is fused into the extractor (`k = fold ? 2 : 4`), and its 2:1 rate is
  net **1,22× better** than fold-off once σ is accounted for;
- chasing down the window/gap split (D2);
- **adaptive bias correction as a replacement for the XOR fold** (proposed 2026-08-26): estimate p̂ by
  EWMA over the raw LSB stream and standardise each run on `μ = 200·nseg·p̂`,
  `σ = √(200·nseg·p̂(1−p̂))` instead of the fixed 0,5 / `GCP_SEGMENT_SD`. It is a LOCATION fix for a
  SCALE failure and cannot work, quantified against D17's own re-test data:
  at the measured fold-off p̂ = 0,493375 the σ factor `√(p̂(1−p̂)/0,25)` is 0,999912 — a correction of
  **8,8e-5 where 1/1,7233 = 0,58, i.e. −42 %, is needed. Short by 4800×.** The overdispersion is not
  binomial, so no binomial σ built from p̂ reaches it; the entropy channel (z_h −201,7) says it is
  spectral structure, which no rescaling of location or scale touches at all.
  Three further reasons, each independently sufficient: **(a)** p̂ is estimated from the same data, so
  the EWMA subtracts the signal — at α = 0,001 and ~3,2 s/run its time constant is **53 min, 3,6×
  slower than the 15-min block**, i.e. a second, un-pre-registered centring on top of D8, removing
  more than block centring already costs, not less. **(b)** A per-node private p̂ trajectory makes
  `z0..z3` four different statistics, which breaks the Stouffer combine and would feed the p̂ dynamics
  into `PairAcc` as correlation — exactly what the shared primitive in `elotto_gcp` exists to prevent
  (D37) — and it moves every stored z, splitting the pooling table (D1).
  **(c)** The bit-rate premise is wrong even on its own terms: fold-off buys 2× nseg per unit time,
  worth √2 = 1,414 in z resolution, against a z that is 1,7233× too wide — **net 1,22× worse.**
  ⚠ With the fold ON the correction is pointless in the other direction: at p̂ = 0,499942 the σ factor
  is 1 − 6,7e-9. The residual LOCATION term is real (−0,32 z/run) but block centring already removes
  it, in the pre-registered way;
- a `docs/data/README.md` index of session generations (user, 2026-08-19: the past will be consulted
  when needed);
- deleting session data from 2026-08-05 on — each of those is either cited evidence or the only
  dataset of its instrument generation, and `_profile/` was committed for exactly the opposite
  reason. (The blanket form of this rule was overruled on 2026-08-20; see 🗑 below.)

⏸ **Deferred by the user — do not start unasked:** the attended-vs-unattended (focus) comparison.

🗑 **Deleted 2026-08-20 (user), and unrecoverable:** `docs/data/2026-07-30_ladders_dc_light/` and
`docs/data/2026-07-30_run5_new_hw/`. v2-era, unpoolable under every split in the pooling table, and
the only record of exposures 4 and 8 *passing* a sweep before the dark-end gate (`exp 4`,
`mean_px 3,97`, `pass:true`) — the counterpart to D18. `git filter-repo` removed them from the whole
of `master`'s history and the result was force-pushed, so no commit here or on GitHub contains them.
The one surviving copy is the local branch `backup/pre-rewrite-2026-08-20`, never pushed and no
longer-lived than this clone. **Re-measure, do not try to recover.**
⚠ The rewrite renumbered all 30 commits from the old `18074b5` on. **A commit hash quoted in a note,
log or firmware build older than 2026-08-20 does not resolve** — match those by ELF SHA.

🗑 **Deleted:** `docs/data/_live_now_*`, a superseded partial pull of the 08-19 session. Its
`.gitignore` entry stays, because that is the name the next live pull will take.

### D46 — The sweep chooses BEFORE the fold, and keeps what works (2026-08-27)

**The criterion was measured on the wrong side of the fold.** `cal_gate()`/selection read
|bias − 0,5| on the emitted stream. The fold maps a raw bias ε to ~2ε², so at ε = 5,4e-3 the folded
residual is 5,9e-5 against a window SE(bias) of 2,3e-4 — every certified rung sits four times below
the noise floor of its own measurement. The master's ladder, distance from 0,5 in units of 1e-3:

| exp | 4 | 8 | 16 | 32 | **64** | 128 | 256 | 512 |
|---|---|---|---|---|---|---|---|---|
| raw | 11,7 | 8,4 | 5,4 | 2,4 | **0,5** | 5,5 | 10,3 | 26,8 |
| folded | | | 0,03 | 0,21 | 0,04 | | | |

A clean U with one minimum before the fold; noise after it. The sweep picked exposure 16 over 64 by
**6e-6**, a fortieth of its own standard error.

**What that cost, measured over the 2026-08-27 session (50 blocks, 3150 items).** Per-block master
offset by the rung the sweep had chosen:

| rung | blocks | mean offset | SE |
|---|---|---|---|
| 16 | 10 | **−0,643** | 0,095 |
| 32 | 15 | −0,321 | 0,047 |
| 64 | 25 | −0,057 | 0,030 |

Monotone, ~10× between the ends, and the choice between them was a coin toss re-thrown every 15
minutes. `drift_t` — the regression of the MASTER's per-block mean, so literally this quantity — ran
to 8,4 and flagged `null_flags` DRIFT in **17 of 50 blocks**, all of them attributed to the master by
`nb_attribute()` in 13 cases. Nothing was faulty; the operating point moved underneath the pass.

**Mechanism at the dark end.** Marginal bias is not what survives: 2ε² at ε = 5,4e-3 is 5,9e-5,
positive, while the measured operational offset is −1,4e-4. What survives is CORRELATION inside the
fold pair. `zero_diff` runs 0,0782 → 0,0928 → 0,1085 over exposures 64 → 32 → 16: a zero pixel
difference has a deterministic LSB of 0 and those zeros cluster spatially, so the two pixels of a
fold pair are both zero together. Monotone in the same direction as the offset, and it is the same
physics `[D18]` gates the bottom two rungs on, one rung higher up.

**Three changes.**

1. **Selection key is `|raw_bias − 0,5|`** (`cal_key()`), the monobit distance before the fold.
   Formally right as well as empirically: the entropy deficit of a bit stream is bias plus
   dependence, and monobit IS the first-order term. `cal_key_se()` is its own SE over `raw_bits`.
2. **Hysteresis.** The incumbent rung — the one in force when the sweep STARTS, so a manual
   `/expose` gets one sweep of protection — is kept unless a challenger beats it by
   `CAL_KEEP_MARGIN_K` 3 standard errors of the challenger's own measurement. Measured, not a fixed
   fraction, so a longer window makes the rule pickier by itself. Verified on hardware the same day:
   `.103` held exposure 16 against a rung 1e-4 better (`kept=true` in `/calibrate`).
3. **Pre-fold dispersion gate**, `CAM_CAL_FAIL_RSIG`. The folded σ gate has the identical blindness:
   it certified `.155`/256 at raw σ 1,3405 (folded 1,0202) and `.145`/64 at 1,2964 (folded 0,9741).
   A front end dispersed by a third reads clean after the fold — and that is the stream `?wpre=`
   ranks on `[D45]`.

⛔ **Both absolute pre-fold bars are dead ends. Do not re-fit them.** A 6,0e-3 bar on raw bias and a
1,20 bar on raw σ were cut in the measured gap of the first sweep. The next sweep, **25 minutes
later with nothing touched**, moved every node:

| node | sweep 1 | sweep 2 | sweep 3 |
|---|---|---|---|
| master | 1,13 | 3,42 | 0,31 |
| .155 | 1,94 | 7,63 | 2,37 |
| .145 | 0,51 | 2,14 | 2,14 |
| .103 | 3,71 | **0,24** | 0,14 |

Not common-mode — three worse, one fifteen times better — and not monotone. `.103`'s raw σ ran
1,00 → 1,29 → 0,98 across the same three sweeps on the same rungs. Under the fitted bars `.155` came
back **CERTIFIED-EMPTY on all nine rungs** with a healthy camera, and `.103`'s entire good ladder
would have been rejected. The pre-fold statistics are non-stationary per node on a timescale of
minutes; only their SHAPE across a ladder is stable. Hence: **raw bias selects and never gates**, and
the dispersion bar is RELATIVE — `CAL_RAW_SIGMA_K` 1,35 × the lowest raw σ among rungs clearing every
other gate, computed after the ladder is complete. By construction it cannot reject the rung it is
computed from, so certifying-empty is impossible.
⚠ On both measured sweeps the relative bar rejects nothing the folded σ gate did not already reject.
Its value is prospective. It does NOT catch the two rungs that motivated it (`.155`/256 is a ratio of
1,25, inside K); tightening K to catch them would put the bar under the drift above. That trade is
made in favour of never blanking a node.
⚠ `CAL_RAW_SIGMA_K` is one-sided. The folded bar is two-sided because an under-dispersed folded
stream is as wrong as an over-dispersed one; every physical failure of the raw stream inflates it.

**The runs channel.** A NIST runs statistic over the pre-fold stream, conditioned on the OBSERVED
proportion of ones — NIST refuses the test unless |π − ½| ≤ 2/√n, which at these bit counts is ~6e-4
against a raw bias missing 0,5 by up to 5e-3, so every rung would be refused. Conditioning makes it
measure clustering GIVEN the imbalance, i.e. the second term of the entropy deficit, which monobit
cannot see. Negative = the bits clump.

It answered its own question in one sweep. On FAILING rungs it is enormous (`.103` −157 at exposure
128, −416 at 256). Among CERTIFIED rungs, where the choice is actually made, all four nodes sat
between −1,4 and +1,7 — **it does not order them**. So it is a gate candidate, never a ranking key,
and a gate only has to run where the gating happens. `s_raw_runs_on` is armed by `camera_calibrate()`
and cleared on every exit path including `aborted`.
⚠ Armed permanently it cost the array **5,38 Mbit/s against 5,71**. `/camtest` priced it at +5,9 %
and +19 % on two runs of the same binary — the self-test benchmarks while the capture task is
extracting, so its `ns_*` vary ~10 % and the delivered bit rate is the honest number, not the
stopwatch. ⚠ During a measurement window `raw_runs_z` publishes 0,0: that is "not armed", not
"perfectly random". `raw_trans` is what says which.
⚠ Both extractors count it and `cam_extract_selftest()` compares transitions AND the carried bit
(`what` 9) — the reference walks bit by bit, the bulk path derives four transitions from a nibble
plus a carried bit, so it is a real comparison. Verified on hardware: 6/6 cases equal.

**Nodes 2026-08-27 after the change**, all certified: master 64, `.103` 16, `.145` 32, `.155` 128;
idle rate back to 5,709–5,716 Mbit/s.

**Not settled.** `mean_px` is still not recorded per block — `LoopStat` carries `cam_exp`, `cam_gain`,
`cam_bias`, `die_temp` but not the light level, and `/calibrate` holds only the last sweep. It is the
one covariate that could say whether the non-stationarity above is the lamp, and it is missing.

---

## Where the rest of the history lives

Superseded design notes, the pre-2026-07-29 optics measurements and the v2 loop/ranking era were
removed from CLAUDE.md and `docs/PLAN.md` on 2026-08-17 and remain in git history at `998c7ab`:
`git show 998c7ab:CLAUDE.md`, `git show 998c7ab:docs/PLAN.md`.

`docs/PLAN_4NODE.md` and `docs/PLAN_NETWORK.md` were deleted earlier and are at `8e134e5`. The source
comments that used to cite them by name were cleaned of those citations on 2026-08-20 — the prose they
carried stands on its own, and a reference no reader can follow is worse than none.

⚠ **`§1.x` in a source comment is NOT such a dead reference**: those resolve to
[`PLAN_HISTORY.md`](PLAN_HISTORY.md), which still carries §1.1–§1.17. They are deliberately bare — the
file is the only thing left numbering sections that way.

The pre-2026-08-19 CLAUDE.md, which carried everything above inline, is at `97e44ce`.

⚠ Every hash on this page is post-rewrite. Anything written before 2026-08-20 quotes the old numbering
and will not resolve.
