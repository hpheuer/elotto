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

### D26 — The four soft-float calls per segment stay
2026-08-19. The P4 FPU is single-precision, so `(ones - 100.0) / 7.07106781` compiles to `__floatsidf`
+ `__subdf3` + `__divdf3` + `__adddf3`, together costing more than the popcounts did. Both obvious
cures — multiplying by the reciprocal, or summing `ones` and dividing once — are arithmetically equal
and **not bit-identical**, so either would shift the last bits of every z in the archive. The one safe
saving left untaken is making the mean subtraction integer, exact for `ones` in [0,200].

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

Consequences elsewhere: `runs_completed` now means ROWS HELD and `items_done` is the session count
(`[D41]`'s archive is what shrinks, not the statistics); `LOOP_HIST` went 128 → 1024, because at 52
blocks per 11,6 h the old value was ~28 h and the first session able to outlive the buffer would
have gone blind there instead.

**Verified by replay, not yet on hardware**: exercising it needs a session that actually fills 8000
items. The cheapest path is 6-of-49 unlimited at `maxruns=5005`, where the second round boundary
cannot fit and compaction fires after ~3,7 h at `?run=1`.

---

## Dropped and deferred

⛔ **Dropped by decision — do not re-propose:**
- the node-drop test, the camera-fault/reboot path, the camera-stall abort;
- restoring the master to USB power (D31);
- raising `CAM_BUF_COUNT` (D24 — measured, no effect);
- the second-core split for extraction (D23 — no headroom left at idle);
- reintroducing the on-chip TRNG in any form;
- chasing down the window/gap split (D2);
- a `docs/data/README.md` index of session generations (user, 2026-08-19: the past will be consulted
  when needed);
- deleting old session data — 2,9 MB in total, git keeps it anyway, and `_profile/` was committed for
  exactly the opposite reason. ⚠ Partly overruled by the user 2026-08-20: the two v2-era 2026-07-30
  directories were deleted (see 🗑 below). The rule still holds for everything from 2026-08-05 on —
  each of those is either cited evidence or the only dataset of its instrument generation.

⏸ **Deferred by the user — do not start unasked:** the attended-vs-unattended (focus) comparison.

🗑 **Deleted 2026-08-20 (user):** `docs/data/2026-07-30_ladders_dc_light/` and
`docs/data/2026-07-30_run5_new_hw/` — v2-era (`rank="cum"`), 3,29 Mbit/s, 44 KB together, referenced
from nowhere including the archive table in CLAUDE.md, and unpoolable with anything under any of the
splits in the pooling table. They were the only record of exposures 4 and 8 *passing* a sweep before
the dark-end gate (`exp 4`, `mean_px 3,97`, `pass:true`), i.e. the counterpart to D18.
⚠ **That record is gone.** On the user's explicit instruction the same day, `git filter-repo` removed
both directories from the whole of `master`'s history and the result was force-pushed, so no commit
here or on GitHub still contains them. The only surviving copy is the local branch
`backup/pre-rewrite-2026-08-20` (old `29b50ea`), which is never pushed and will not outlive this
clone. If the pre-gate ladder data is ever wanted again, it must be re-measured, not recovered.
⚠ The rewrite renumbered every commit from the old `18074b5` onward — 30 of them. Any commit hash
quoted in a note, log or firmware build older than 2026-08-20 no longer resolves.

🗑 **Deleted:** `docs/data/_live_now_*`, a superseded partial pull of the 08-19 session whose 123
provisional rows are all superseded in the complete archive. Its `.gitignore` entry stays, because
that is the name the next live pull will take.

---

## Where the rest of the history lives

Superseded design notes, the pre-2026-07-29 optics measurements and the v2 loop/ranking era were
removed from CLAUDE.md and `docs/PLAN.md` on 2026-08-17 and remain in git history at `998c7ab`:
`git show 998c7ab:CLAUDE.md`, `git show 998c7ab:docs/PLAN.md`. (Was `144ed5e` before the 2026-08-20
history rewrite.)

`docs/PLAN_4NODE.md` and `docs/PLAN_NETWORK.md` were deleted earlier and are at `8e134e5`. Source
comments still cite those two by name; the citations are historical and resolve to git history.

The pre-2026-08-19 CLAUDE.md, which carried everything above inline, is at `97e44ce`
(`61bb92d` before the 2026-08-20 history rewrite).
