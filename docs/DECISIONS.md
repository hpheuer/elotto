# elotto — decision log

CLAUDE.md holds the RULES. This file holds the EVIDENCE behind them: what was measured, what was
tried and rejected, and what it cost. Split out on 2026-08-19 because CLAUDE.md is loaded into
context every session and had grown to 830 lines, more than half of it history.

Entries are append-only by number and cited from CLAUDE.md as `[D<n>]`. Bodies may be shortened —
full prose at `git show 1e62bca:docs/DECISIONS.md` (pre-2026-08-28-docs-trim). Working-tree
entries after that commit keep enough text that the decision survives. A rule and its entry must
be changed together — a rule whose evidence has moved on is worse than no rule.

⚠ Everything here is dated. A measurement describes the instrument that made it; see D1.
**Live instrument is D65** (LSB z + concordance). **Live ranking scale is D68** (block σ),
unbounded and per-item weighted (D75), one block per round (D76).
Earlier ranking-key formulae are evidence, not the current key.

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
| 2026-08-31 | ranking key uses block σ (D68) | tables; `z_raw`/`z_ctr` still pool |

Also: attended (`focus=1`) and unattended sessions are separate arms and are never pooled.
Post-D66 is always unattended (`focus=off`). v3 and any v2.x session are separate instruments
outright.

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
**Superseded as a rule by D66** (no observer gate). Evidence stands: 2026-08-13, the UI sent
`confirm=1` unconditionally and the gate had no timeout, so an unattended 5005-item run parked
behind a Start button nobody was there to press: **37,9 h wall against 12,2 h of measuring**.

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

### D9 — Nearest-zero means nearest the PASS MEAN (2026-08-05)
**Dead.** v3 has no ranking modes and `results_near_mean()` is gone with them. Text:
`git show 1e62bca:docs/DECISIONS.md`.

### D10 — The studentized-view checkbox of PLAN.md §2.3 was never built
**Moot since D68:** `Z*` **is** the key, in block-σ units. Not a feature, and not a deferred one.

---

## Node health

### D11 — Soft-down trips on σ alone; |mean| only reports
**Entscheidung:** |mean|-Trip (`NODE_MEAN_SOFT` 1,50) entfernt 2026-08-19 — eine Session lief 50 %
der Items bei k < 4 ohne sichtbaren Fault. Offsets kommen vom Sweep (dunkle Stufen), Centring
entfernt sie schon aus `z_ctr`, und der Schwellwert war nicht run-längen-invariant. `mflag` bleibt
Report, nie Exclusion. 2026-08-26 bestätigt auf gated Master-Rung: pure Location, Soft-down richtig
stumm. 2026-08-11 (ohne Centring) ist kein Gegenbeispiel.

**Zahlen:**
| exposure | 4 | 8 | 16 | 32 | 64 | 128 |
|---|---|---|---|---|---|---|
| mean offset | −1,86 | −0,78 | −0,05 | +0,09 | −0,07 | −0,04 |

exp ≤ 8: t = −4,0. Master gated (7354 Items): mean −0,465 / σ 0,995 / mflag 3 / trip 0; raw
Stouffer −39,8 → z_ctr 0. Replay: 24/29 Blöcke bei k = 4 statt 12.

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
**Entscheidung:** Closed 2026-08-19. σ ~1,04 stayed with slave1 after the camera swap; slave2 with
that camera is the lowest of four. Tolerating costs ~1 % combined σ; dropping costs 13 %. Past long
exclusions were mostly D12, not condition.

---

## Calibration

### D16 — Rung selection: lowest |bias−0,5| among candidates that clear every gate
Selection used to take the FASTEST passing candidate, on the assumption that a shorter exposure means
a faster frame rate means more bits. That assumption is dead: the bit rate is CPU-bound, and a full
sweep measures 3,217–3,293 Mbit/s across exposure 4..512 — a 2,4 % spread, i.e. noise. The tie-break
was comparing eight identical numbers and picking whichever measured highest. Cost, measured: the
master picked exposure 16 (bias −3,7e-4) over 32 (−2,1e-4) on a 0,7 % rate difference, and across one
5-loop session its choice wandered 128 → 256 → 8 → 128 → 128, once landing on a rung a standalone
sweep had FAILED at −1,21e-3.

A σ MARGIN rule (2026-07-27) sat on top of this for a year: a rung had to clear `CAL_SIGMA_TOL` by
half its tolerance to be selectable, because bias alone is not the property that hurts. Over a
200-loop session, node .145 sat on exposure 32 in 91 of 127 logged loops and produced every σ
excursion there (per-loop σ SD 0,245, max 3,008), while on exposure 16 it was indistinguishable from
healthy (SD 0,082 against 0,089 expected from sampling). Its exposure-32 rung sat ON the σ gate: it
passed some sweeps and failed others, and when it passed its bias often measured best — so the
bias-only rule selected it. A rung that scrapes a gate is a rung on a cliff.

⛔ **The margin rule is GONE (2026-09-02).** D65 deleted `CAL_SIGMA_TOL` with the second stream it
read, so there was no gate left for the margin to sit on; the code had already stopped applying it
and only the comment survived. Dispersion is now gated by `CAL_RAW_SIGMA_K` 1,35, relative to the
ladder's own best rung (D46). Do not re-add an absolute margin on top of a relative bar — the bar
already travels with the node's level, which is the whole reason it is relative.
⚠ Selection key moved to LSB bias in D46. The cliff-edge failure mode the margin addressed is NOT
abolished — the sweep's σ does not always predict the session's, and soft-down (D12) is what catches
it now.

### D17 — LSB-as-is has no σ≈1 null (2026-07-26 / 2026-08-26)
**Superseded as a rule by D65** (LSB bits are measured as-is). Evidence stands: LSB bias is non-stationary;
σ blows out (1,04 → 2,15; rung 128: 1,72 vs 1,01). Recentring takes the mean, not the scale.
Pass health after D65 is the **measured** σ.

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

### D22 — 3,42 → 5,71 Mbit/s per node (2026-08-18)
→ **D23.** `-O2` + inline popcount + word-wise extraction, bit-for-bit the same stream.
⚠ Both figures are the adjacent-pixel-XOR instrument, which D65 turned off.

### D23 — The idle ceiling is the sensor, and we are at 98,5 % of it
The old "71 % of the ceiling" figure assumed 50 fps from the datasheet. `fps_raw`
(`camera_fps_probe()`, run by `/camtest` before the benchmark so it sees an idle CPU) times the
sensor with extraction stopped: **36,11 and 36,22 fps** on two runs → a pair every 55,2–55,4 ms
against a live 56,0 ms, i.e. 18,1 pairs/s × 320.000 bit = **5,80 Mbit/s** against 5,71 measured.

The measured PSRAM read floor is 7,1–7,9 cycles/pixel against ~21 for extraction+statistics.
⚠ Those figures are the pre-D65 stream (D22). With the adjacent-pixel XOR off the word rate
doubles: idle production measured **~7,4 Mbit/s**. The ceiling is still the sensor.

### D24 — Why the last two extraction changes bought nothing (answered 2026-08-18)
**Entscheidung:** Idle loop is DQBUF-bound; CPU savings vanish into wait. `CAM_BUF_COUNT` 4→8 moved
the wait/extract split only — reverted, do not re-propose. Ceiling is the sensor (D23).

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
**Entscheidung:** Baseline needed the same onset flush so `LoopStat.base` matched `raw_m`
provenance. Baseline phase itself is DELETED — see D48.

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
**Entscheidung:** At a closed round boundary, compact instead of ending: keep
`PASS_KEEP_PER_TABLE` (16) extremes per table, the rest into moments. Top/Bottom exact;
Nearest-zero effectively exact (centring pins block means at 0). Only when the next round would not
fit. `round_base` = results[] index; `round_item_base` = items_done — they diverge after compaction;
using the wrong one double-counted survivors on first hardware run (fixed 2026-08-20).
`runs_completed` = rows held; `items_done` = session count. `LOOP_HIST` 128 → 1024.

**Zahlen:** Replay 8000 → 48 rows, σ/χ² bit-identical. Verified: `compacted` 15828,
`pass_n_valid` == `completed` == 17766.

### D43 — The spectral structure is DRIFT, not a row line; the LSB monitor (2026-08-26)
**Entscheidung:** `/specdump` shows excess at the lowest bins (drift inside the window), not at the
predicted row-rate bin 256 — no spatial period in the LSB stream. Exposure is the variable; adjacent-pixel XOR
suppresses drift without removing it. Stride/shuffle merges and spatial shuffling are dead. LSB
monitor (`raw_bias`/`raw_sigma`) published; `GET /diagjson?all=1` is the array collector (409 while
measuring). selection was noise-limited — resolved in D46.

**Zahlen:** exp128 LSB-as-is bin1 23,81× / XOR-on 8,87×; at exp64 both flat.

### D45 — The LSB z: the channel adjacent-pixel XOR throws away (2026-08-26)
**Superseded as a ranking rule by D65.** Uncentred LSB z ranks nodes (offsets to −47σ) —
block centring is load-bearing. `NUM_RUNS` 7200 (Euro 7920 needs one compaction). `pre_w` still
splits the pooling table for TABLES.

## Dropped and deferred

⛔ **Dropped by decision — do not re-propose:**
- the node-drop test, the camera-fault/reboot path, the camera-stall abort;
- restoring the master to USB power (D31);
- raising `CAM_BUF_COUNT` (D24 — measured, no effect);
- the second-core split for extraction (D23 — no headroom left at idle);
- reintroducing the on-chip TRNG in any form — **including an LFSR fed from the camera bits**
  (proposed 2026-08-26). An LFSR whose feedback is XORed with the raw stream IS the canonical
  whitened-hardware-RNG construction, so it falls under this line; it is named separately because the
  line did not read as covering it.
  ⛔ What it costs is the instrument: fed a **frozen camera** (every diff zero, every LSB
  deterministically 0) the LFSR emits bias 0,499993, σ 1,0001, autocorr 0,0004 — **it passes every
  gate this project has**, where the raw and LSB paths both read bias 0,000000 / σ 0,0000 and are
  caught instantly. It also has STATE, so it smears a time-localised deviation over the register
  length and beyond; and it puts a PRNG in the z path, which `fast_rng()` is explicitly kept out of.
  ⚠ The 2026-08-26 text also priced the LFSR against the adjacent-pixel XOR ("buys nothing XOR does
  not deliver", "1,22× better") and against the entropy channel `z_h`. **D65 turned that XOR off and
  D53 deleted `z_h`**, so those comparisons are void — the frozen-camera argument above does not
  depend on them. Full text: `git show 1e62bca:docs/DECISIONS.md`;
- NIST runs as a ranking channel (tried and deleted, D55 — underdispersed, orthogonal to Pre);
- chasing down the window/gap split (D2);
- **adaptive bias correction** (proposed 2026-08-26): estimate p̂ by EWMA over the raw LSB stream
  and standardise each run on `μ = 200·nseg·p̂`, `σ = √(200·nseg·p̂(1−p̂))` instead of the fixed
  0,5 / `GCP_SEGMENT_SD`. It is a LOCATION fix for a SCALE failure: at the measured LSB p̂ = 0,493375
  the σ factor `√(p̂(1−p̂)/0,25)` is 0,999912, a correction of **8,8e-5 where −42 % is needed —
  short by 4800×**. The overdispersion is not binomial, so no binomial σ built from p̂ reaches it.
  Two further reasons, each independently sufficient: p̂ is estimated from the same data, so the EWMA
  subtracts the signal (at α = 0,001 its time constant is ~53 min against the 15-min block — a second,
  un-pre-registered centring on top of D8); and a per-node private p̂ trajectory makes `z0..z3` four
  different statistics, breaking the Stouffer combine and feeding the p̂ dynamics into `PairAcc` as
  correlation — what the shared primitive exists to prevent (D37) — while moving every stored z (D1).
  ⚠ The original framed this as "a replacement for the adjacent-pixel XOR" and costed it in bit rate
  against XOR-on. **D65 turned the XOR off**, so that framing and its 1,22× are void; the scale
  argument above stands on its own. Full text: `git show 1e62bca:docs/DECISIONS.md`;
- keeping prior-instrument session CSVs in-tree — unpoolable with D65; wiped 2026-08-31 (user).

⛔ **Attended mode deleted (D66, 2026-08-31).** The deferred attended-vs-unattended comparison is closed.

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

### D46 — The sweep chooses on the LSB stream, and keeps what works (2026-08-27)
**Entscheidung:** Selection key is `|raw_bias−0,5|` (LSB); LSB key was noise (exp16 beat 64
by 6e-6 ≈ 1/40 SE). Incumbent kept unless challenger beats by `CAL_KEEP_MARGIN_K` 3 SE. Absolute
LSB bars are dead ends (non-stationary per node; certified-empty a healthy node) — raw bias
selects never gates; raw σ is relative (`CAL_RAW_SIGMA_K` 1,35 × ladder best, one-sided). Runs
statistic armed only inside a sweep (permanently on: 5,38 vs 5,71 Mbit/s).

**Zahlen:** Master offset by rung over 50 blocks: exp16 −0,643 / 32 −0,321 / 64 −0,057.

### D47 — Four health mechanisms, one of them load-bearing; the other three are gone (2026-08-28)
**Entscheidung:** Deleted `null_flags`, `NB` attribution, per-node CUSUM. Soft-down + quarantine
alone affect data. Software publishes numbers, draws no verdict; `PAIR_FLAG_T`/`DRIFT_FLAG_T` are
display hints only. Soft-down still watches σ — at `?wpre=` 0,8 ranking rides LSB;
deliberately not closed in software (answer: light). CSV lost `null_flags=`.

| mechanism | effect on the data |
|---|---|
| soft-down + quarantine | **real** |
| `null_flags` / `NB` / CUSUM | none (removed) |

### D48 — The baseline phase is gone; the scoring key is the pass key (2026-08-28)
**Entscheidung:** Baseline phase DELETED — block centring already gives the drift reference from
~200 runs. Scoring key now uses all three channels via `score_build_keys()`; scoring pass
centres/standardises itself per channel (required for LSB — without it every candidate ran off the
scale). `?baseline=` → 400. Slave still answers `B`. Pooling: split on 2026-08-28.

### D49 — The start form remembers its last values, the API remembers nothing (2026-08-28)
**Entscheidung:** Form fields persist in NVS, served as script on `/`. API defaults untouched. Only
`confirm=1` starts write (UI only) — curl must not overwrite operator weights with API defaults.
Mode not remembered. Fields: measuring time, unlimited + runs per round, score
direction, concordance weight. Focus dropped with D66. Implemented 2026-08-29 (was documented,
the write/read was missing, and focus was the one the operator noticed).

### D50 — The block table records what the camera DID, not what the sweep found (2026-08-28)
**Entscheidung:** Sweep fields (`cam_bias` etc.) are identical across blocks on one setting — a
σ 6,94 block was unattributable. Added during-block `cam_sig` (σ) / `cam_rsig` (LSB σ) /
`cam_px` (mean pixel). `cam_rsig` recorded never gated. UI: separate `cam σ` (LSB) and `z σ`
columns, no fallback. `cam_px` 0 = not reported.

**Zahlen:** 48 B/row × `LOOP_HIST` 1024 = 48 KB PSRAM.

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

---

⚠ **Korrigiert durch D62 (2026-08-30):** `cam_sig`/`cam_rsig`/`cam_px` are cumulative
since the last `camera_stats_reset()`, and only the sweep and `POST /expose` perform one —
never a block close. Up to three blocks share one accumulation, and a block straight after a
sweep carries a shorter and noisier one. They do still describe the camera rather than the
last sweep's stored result, which is what this entry was for, but they do NOT isolate a
block. Per-item `w0..w3` (D62) is what localises.

### D51 — Measuring-time floor is 0,5 s (2026-08-28)

User: allow `?run=0,5` in the UI and API. Previously fixed at 1..5 s (2026-08-18) so a request
outside that range could not silently become a different experiment.

At the live cal (`RUN_SEGS_REF` 70513 / `RUN_MS_REF` 2703) half a second is ~13043 segments /
~2,61 Mbit — half of `?run=1`, still above `EL_SEG_MIN`. Auto-gap stays floored at `GAP_S_MIN`
0,5 s (40 % of 0,5 would be 0,2).

---

### D52 — Bright-end and LSB-bias gates are publish-only (2026-08-28)

**Entscheidung:** `CAM_CAL_FAIL_LIGHT` (`CAL_MAX_MEAN_PX` 100) and `CAM_CAL_FAIL_BIAS`
(`|bias−0,5|` vs `cal_bias_bar`) no longer reject a rung. Selection stays LSB
`|raw_bias−0,5|`; rejection stays σ / autocorr / dark / zero_diff / stuck / bits / RSIG.

**Warum:** LIGHT rejected the best measured rung (exp 64, mean_px 68,7) while saturation already
fails SIGMA `[D20]`. LSB bias is noise-limited after adjacent-pixel XOR and duplicates what the LSB
key already ranks `[D19]``[D46]`. Constants and `cal_bias_bar()` remain for `/calibrate` readability
and old CSV comparability; bit values stay defined as legacy.

### D53 — Spectral-entropy channel deleted (2026-08-28)
**Entscheidung:** Gone. Do not re-open.

### D54 — Runs ranking
→ **D55.**

### D55 — Runs ranking: tried 2026-08-28, deleted 2026-08-29
**Entscheidung:** Gone. Sweep still publishes `raw_runs_z` per rung, not a gate (D46). Underdispersed and orthogonal to the z outliers.

### D56 — Half-window concordance; compact 100 extremes (2026-08-29)
**Entscheidung:** Concordance = leave-one-out Stouffer of per-node half-window z. Split at nseg/2;
same sign → `√2·min(|h1|,|h2|)`; else 0; drop the loudest node. Unlimited rounds compact every
boundary to the 100 most extreme `|rank_key|` (both tails). Live as of D65: z + concordance.

### D57 — No corrected-p / Bonferroni (2026-08-29)
**Entscheidung:** Gone. Pass mean/σ/χ² stay as instrument health.

### D58 — Own-σ per ranking channel (2026-08-29)
**Entscheidung (still live, scale moved by D68):** each ranking term is standardised by **its own**
measured σ. One shared σ silently reweights the key. Since D68 that σ is the item's **block** σ,
not a session-wide channel σ (deleted by D72). Z* **is** the key; it no longer studentises on session
the session key moments that D71 later deleted.

### D59 — Gain 256 tried and reverted; slave0's light was the real fix (2026-08-30)
**Entscheidung:** `CONFIG_ELOTTO_CAM_REG_GAIN` stays at **1023**. The operator's physical
dimming of slave0 stays and is the change that worked. `raw_words`/`mid_words` in
`gcp_zscore_pre()` are derived (`7*nseg`) instead of counted per word — bit-identical, no
pooling split. `pre_n` is seeded with the compaction moments like `pass_n_valid`.

**Warum das Licht:** slave0 sat at ~8x the illumination of master and slave2, so the sweep
could only reach exp 4/8/16 and certified 3 of 9 rungs at `raw_sigma` 1,36 — the worst node
in the array, and the one dragging the combined LSB σ. After dimming by ~6,7x it
certifies **6 of 9 rungs at 1,032**, the best node. Nothing in software did that.

**Warum der Gain zurückgenommen wurde — und die Lehre über die Messmethode.** Single
`POST /expose` readings, 25 s apart, said `raw_sigma` fell from 1,36 to 1,00 on every node at
gain 256, apparently independent of exposure. It was noise: two readings at **identical**
settings minutes apart gave 1,211 and 1,025. `raw_sigma` is non-stationary per node on a
timescale of minutes (D46) and a single rung is not evidence about it.

The sweep — the whole ladder measured back to back, which is the only comparison that holds —
says the opposite:

| node | gain 256, certified rungs | gain 1023 |
|---|---|---|
| master | **0 of 9** (every rung `CAM_CAL_FAIL_ZDIFF`) | 3 (exp 64, rsig 1,032) |
| slave0 | 1 | **6** (exp 128, rsig 1,032) |
| slave1 | 1 | 4 (exp 64, rsig 1,203) |
| slave2 | 1 | 3 (exp 128, rsig 1,145) |

Lower gain means smaller frame-to-frame differences, so `zero_diff` roughly doubled and the
master — the darkest node — could not certify anything. Bit rate was unaffected either way
(5,715 Mbit/s), so the trade bought nothing and cost the master its certification.

**Was NICHT gemacht wurde:** the batched ring read (one `camera_read_words_raw()` per segment
instead of seven single calls, saving six sevenths of 3,65 M memory fences per run). Held back
deliberately, the same way D26 held back the exact soft-float change: two edits to the
extraction path at once cannot be measured apart. The cheap half was done and measured first —
deriving `raw_words` moved `ms_extract` 77,3 → 76,9 ms, **0,5 %**, i.e. nothing. That is the
prior for the batch read too: it is the memory fences and the cross-TU call that would have to
pay, not the arithmetic.

**⚠ Offen: slave1 is now the bright node.** Illumination per exposure unit: master 0,20,
slave0 0,13, slave2 0,22, **slave1 0,48** — and slave1 has the worst `raw_sigma` (1,203) and
sits on the shortest rung (exp 64, later exp 16). The same dimming that fixed slave0 is
indicated, by roughly a factor of 2–3.

### D60 — The node table shows CONSUMPTION, not production (2026-08-30)
**Entscheidung:** `cam_stats_t` gains `consume_mbit_per_sec`: bits handed to a caller of
`camera_read_word()`/`_raw()`, over the microseconds spent reading them. `gcp_zscore_pre()`
reports one span per run via `camera_note_consumed()` — two `esp_timer_get_time()` calls per
run, not one per word. Published in `/diag`, `/diagjson`, `/status` (`cam_cons_mbit`) and as
`,cons=` on the `D:` reply; the UI node table and `/diag` show it, falling back to a
parenthesised production rate for a node too old to report it. Counted in **32 bits per ring
word**, the same unit `mbit_per_sec` uses — the 64 LSB pixels behind a word are
coverage, not a second throughput, and mixing the two put double-scaled numbers in one column.

**Warum:** `mbit_per_sec` is what extraction wrote into the ring, discarded words included.
On 2026-08-30 the master read 5,71 against 3,34 on the slaves and it meant nothing about the
devices: the master's consumer was the slower one, so its ring overflowed (345 M drops, ~0
waits) while the slaves consumed everything they produced and waited constantly (162 k waits).
The operator wanted device performance; that number was measuring the size of the surplus.

Measured after the change, all four nodes: production 5,711..5,715, consumption 5,768..5,774.
They read the same words in the same window and are equally fast — which is the answer, and
was invisible before.

**⚠ Was hier als OFFEN stand, ist seit D61 geklärt: the window length is bimodal.** At
identical parameters (`run=5`, 130435 segs) `focus_win_ms` came back 5134..5163 ms in some
sessions and 10201..10210 ms in others, with production 5,71 against 3,34 to match. Verified
NOT a regression: the committed 25cb867 build measured 10201 ms on all four in one session and
5139 in another. Whatever selects the mode, it survives a reflash and doubles the measuring
time per item. `focus_win_ms` is the master's OWN read span (`focus_off()` runs before
`nodes_collect()`), so it is not slave latency. The consumption rate is the instrument for
chasing this: it separates read speed from surplus, which nothing did before.

**Aufgelöst durch D61:** `elotto_task` was created unpinned on every `/start` and landed on
`cam_task`'s core about one session in three. The consumption rate did exactly the job claimed
for it here — the halving from 5,774 to 2,881 is what identified the mechanism.

### D61 — The extraction task and its consumer are PINNED to different cores (2026-08-30)
**Entscheidung:** `cam_task`, `elotto_task`, the master's `ws_task` and the slave's `link_task`
are created with `xTaskCreatePinnedToCore()` instead of `xTaskCreate()`. `ELOTTO_CAM_TASK_CORE`
(1) and `ELOTTO_CAM_CONSUMER_CORE` (0) in `camera.h` are the single definition of the split, so
neither firmware spells out a core id of its own. Priorities are unchanged.

**Warum:** this closes D60's open bimodal window. `xTaskCreate()` is `tskNO_AFFINITY` and the P4
has two cores, so the scheduler chose a core for each task. The master creates `elotto_task`
fresh on **every** `/start`, so the choice was re-rolled per session: on the free core the two
tasks had one core each, on the camera's core they shared one and both halved. Measured on the
master, same firmware, same parameters:

| | ms_extract | ms_wait | ms_pair | production | consumption | focus_win_ms |
|---|---|---|---|---|---|---|
| one core each | 46,9 | 6,9 | 56,1 | 5,71 | 5,77 | 5134..5192 |
| sharing a core | 80,0 | 17,4 | 100,1 | 3,20 | 2,88 | 10186..10229 |

Consumption halving **exactly** (5,774 -> 2,881) is the signature: two tasks that each had a core
now split one. `ms_wait` rises with it because `CAM_BUF_COUNT` is 4 and the loop holds two of
them — a stretched extraction outlasts the two free buffers and capture stalls, so the next
`DQBUF` waits longer.

**Wie es eingekreist wurde,** because the wrong answers were all plausible and each was killed by
a measurement rather than an argument:

- **Not the data.** The bulk loop in `cam_extract_fast()` has no data-dependent branch at all
  (`zeros` and `psum` are multiply tricks), so its instruction count per frame is constant.
- **Not the exposure.** The whole ladder, three slaves, 30 s per rung: `ms_extract` 46,15..46,21
  across all eight rungs at `mean_px` 3,1..110,6 and `zero_diff` 0,036..0,178. This also retires
  the "45,7 at exp 16 against 59,1 at exp 64" note in `camera.c`, which was almost certainly an
  idle reading against a loaded one.
- **Not anything set at boot** — not PSRAM speed, not cache, not buffer placement. 10 of 10
  idle boots ran fast, and in the mixed test the idle reading taken **before** each session was
  fast even in the sessions that then came out slow.
- **Not the array.** Master only; all three slaves measured 46,7..47,0 in the same slow sessions.
- **Not the boot at all.** Two sessions inside ONE boot came out in different modes. That is what
  named the session start, and `/start` is where `elotto_task` is created.

**Verifiziert:** 4 of 11 session starts were slow before; 20 of 20 after, `ms_extract`
46,65..46,72, `focus_win_ms` 5126..5161. Under the old rate a clean run of 20 has p = 1,3e-4.
`ms_wait` also improved slightly, 6,9 -> 6,0, which is `ws_task` no longer able to land on the
extraction core.

⚠ The priority rule (consumer above `ELOTTO_CAM_TASK_PRIO`, D24) and this core split are
SEPARATE requirements and both must hold. Priority decides who wins once two tasks are on one
core; the pinning is what keeps them off one core at all.

⚠ A single fast session proves nothing about this class of bug — it was intermittent at
roughly one session in three. Any future change that creates a task in the measuring path must be
re-tested over ~20 starts.

⚠ Sessions before and after this change are not the same instrument where the slow mode
actually struck: those items got the same segment count over twice the wall time. `run_s` and
`run_segs` in the CSV header do not distinguish them — `focus_win_ms` does.

### D62 — Camera sigma per MEASUREMENT, and a jump board that outlives compaction (2026-08-30)
**Entscheidung:** every node measures the per-mini-run sigma of **its own measurement
window** and reports it: a second Welford accumulator in `camera.c`, zeroed where the window's
first bit is produced (the ring-flush branch of `camera_task`), published as
`win_sigma`/`win_sigma_samples`. A slave appends it to its `Z` reply as a tagged `,wsig=`; the
master stores four floats per item in PSRAM (`s_node_wsig`, ~115 KB) and streams them as
`w0..w3` at the end of `/results.csv?all=1`. `/status` and the web page gain a third table: the
**camera-sigma jump board**, the five largest changes from a node's previous measurement to
this one, session-wide, with the measured jump noise (`wsig_sd`) beside them.

**Warum:** `cam_sig` in `/loops` cannot localise anything, and the reason is that it is not a
per-block figure at all. `camera_stats_reset()` runs only inside the sweep and on `POST /expose`
— never at a block close. The value recorded at a block therefore covers everything since
the last sweep, up to three blocks on this rig:

| block | cal_ms | cam_sig |
|---|---|---|
| 40 | 10274 (sweep) | 0,9997 |
| 41 | 0 | 0,9998 |
| **42** | 0 | **1,0566** |
| 43 | 10292 (sweep) | 1,0123 |

The 1,0566 covers blocks 41 **and** 42 together, and a block straight after a sweep has a
shorter accumulation and so a noisier sigma than one at the end of an interval — the figures
are not even comparable with each other. D50's claim that `cam_sig`/`cam_rsig`/`cam_px` describe
what the camera did DURING each block is wrong in that respect; corrected there.

A window carries ~3300 mini-runs at `?run=2`, so a per-item sigma is good to about **+-0,012**
and the jump between two of them to **+-0,017**. Measured on hardware the same day over 80
node-item jumps: **0,0177**. That is the case for doing this per item — at block level a
single disturbed item is averaged with sixty quiet ones and disappears.

**Warum der Sprung und nicht der Pegel:** the level drifts with the operating point, so an
absolute bar would flag a node's rung rather than an event. A jump is differenced against that
node's own previous window and is blind to where it sits. A lasting disturbance gives a jump up
when it starts and one down when it ends; both are real, which is why the board ranks |jump|
and keeps the sign in prev/now.

**Warum eine Tafel und keine dritte Rangliste ueber `results[]`:** results[] is compacted at
every round boundary. On 2026-08-30 the individual rows of the one genuinely disturbed block
were asked for four hours later and were gone — that block had kept none of its 63. A table
computed from `results[]` would show the current round and nothing else. The board is a fixed
`WSIG_TOP_N` array that copies round, index, node and the drawn numbers at the moment the jump
happens, so it survives compaction, round boundaries and a re-scored pool.

⚠ **It is a suspicion list, not a ranking.** The top row is the item whose bits were least
quiet while they were taken, i.e. the z that deserves the LEAST trust — the opposite of
Top-5. It excludes nothing and gates nothing (D47); `counted` says whether that node was still
in the combine, so a reader can tell a disturbance that reached the z from one that did not.

⚠ **One row per NODE, not per item.** Two nodes jumping on the SAME item is the light; one
node alone is that camera. Making that distinction on 2026-08-30 took a manual four-node
`mean_px` comparison across blocks; collapsing the rows would erase it again.

**Kein Mindestsprung, und die Karte ist immer sichtbar.** A `WSIG_MIN_JUMP` of 0,05 stood here
for the first hours and was removed the same day. At the measured jump noise it is only 3,2
sigma, so it fired about once in 700 node-measurements: through a healthy 316-measurement
session the board stayed empty and the card, which hid itself when empty like Top/Bottom do,
never appeared at all. That reads as a broken feature rather than a quiet instrument.

The job a floor was meant to do — stop noise looking like a finding — belongs to `wsig_sd`
and the **x-sigma column** instead, and they do it better: five rows at 2..3 sigma ARE the
quiet session, stated in the units that say so, and a real event pushes one of them into
double digits. `wsig_sd` is accumulated over every jump including the quiet ones — taking
it only from those clearing a floor would have measured the floor. The card now renders even
with nothing on it: an empty board is a statement, and Top/Bottom hide when empty only
because an empty RANKING says nothing. This is not a ranking.

⚠ Filled from the MEASURING pass only. A scoring run has no item identity to name.

**Verifiziert** on hardware in three stages. With the floor still in: 21 items x 4 nodes gave
80 jumps at noise 0,0177, exactly one above 0,05 (node1, item 3802, -0,0512), and exactly
that one on the board with matching prev/now/jump. Over a longer run, 521 items and 2080
node-measurements: `wsig_sd` matched an independent recomputation from the CSV to four
decimals (0,0155 both), and again exactly one archive jump crossed the floor and exactly it
was on the board. After the floor was dropped: five entries by item 17, 1,8..2,7 sigma, i.e.
the board fills from the first measurements and labels itself as quiet. `w0..w3` populated
on every row from all four nodes throughout.

### D63 — A soft-down trip records WHICH measurements carried it (2026-08-31)
**Entscheidung:** when a block trips a node, `record_loop()` captures the `TRIPX_TOP_N` (3)
items of that block whose per-node z sat furthest from the block mean, together with the block
number, the node, the block sigma and mean. Kept in `g_status.trip_hist` (`TRIPX_MAX` 6 per
session, oldest kept), published as `trips` in `/status`, and shown on the page as a fourth
card, **Soft-down origins**. Hidden when nothing tripped. Each title carries wall time from
`t_ms` (board uptime at the trip; UI: `now − (uptime_ms − t_ms)`).

**Warum:** the operator asked which single measurement caused a soft-down. The honest answer is
that none did — sigma is the spread of one node's z over the block's ~63 items and does not
exist until the block closes. But the answerable question behind it, *which measurements made
that spread big*, had no answer either: the block's rows are compacted away one round later.
Twice in two days they were asked for hours afterwards and every row was gone, once for the
only genuinely disturbed block of the session.

So it is taken at the one moment it exists — inside `record_loop()`, where the block's items
are still in `results[]` and their per-node z still in the archive. Same principle as D62's jump
board: copy what names the measurement, do not hope the row survives.

⚠ `dev` is (z - block mean) / block sigma: how far an item sat from the middle of the very
spread it helped create. It is NOT a z against the null. With ~63 items a value near 3 is
ordinary, and **the point is the shape**: one item far out is a single excursion, three close
together mean the block was simply wide.

⚠ The block number is stored 1-based, matching what `/loops` shows. `results[].block` is
0-based, and confusing the two already caused one disturbed block to be read off by one.

**Verifiziert** by temporarily lowering `NODE_SIGMA_SOFT` from 1,25 to 1,02 — a real trip is
otherwise about one per eight hours, which is not a test. Three trips in block 1 each recorded
their three largest contributors with z and dev; the threshold was restored and the node rebuilt
to a byte-identical image (`fw_sha` unchanged from before the test), confirming the revert. The
recorded shape was itself the demonstration: three contributors at 2,1..2,5 sigma close
together, i.e. a wide block rather than one excursion.

### D64 — Every node keeps its own per-window camera log, and a session locks its sensor (2026-08-31)
**Entscheidung:** three changes to the shared camera component, served by master and slaves at
the same paths.

1. **`GET /camlog`** — a 512-entry ring per node, one entry per measurement window, pushed by the
   consumer at the exact point it reads `win_sigma` for the wire. Per entry: `t_ms`, `tag`,
   `wsig`, `wn`, `rsig`, `rbias`, `sig`, `bias`, `px`, `ac1`, `zdiff`. `tag` is the combination
   id on the master, the answered `M` sequence on a slave, **0 for a scoring run**. Oldest-first,
   with a `dropped` counter so a gap can never read as a quiet stretch. Readable **during** a
   session — that is the point.
2. **`GET /linearity[?exp=a,b,c,d][&settle=<ms>]`** — the CLAUDE.md light test as one request.
   Ladders the exposure only (gain carried, per D59), reports `mean_px`, `px_per_exp` and the
   ratio against the previous rung, and restores the entry exposure before replying.
3. **The slave session latch.** `slave_busy()` was `g_measuring`, true only for the ~2 s of a
   window. It is now `g_measuring || session_active()`, where the latch is set by `M`/`K`,
   cleared by `A`, and released after `SESSION_IDLE_MS` (60 s) of silence. `/expose`,
   `/linearity` and `/camtest` answer 409 for the whole session; **OTA deliberately keeps the
   narrow `g_measuring`**, so "abort, then flash" is unchanged.
   `slave_abort()` is now also called at `finalize:` in the master, so a session that ends
   normally releases the latch instead of leaving every slave locked for 60 s.

**Warum:** on 2026-08-31 slave2 threw bursts of z at −31..−62 with per-block sigma up to 20,5 in
blocks 6, 8, 13, 14 and 17, quiet in between. Nothing on the node could say what its camera was
doing in those windows. `/loops` carries `cam_sig`/`cam_rsig`/`cam_px` cumulative **since the
last sweep** (D62), i.e. up to three blocks — it cannot locate anything in time. The per-item
`w0..w3` in the CSV can, but compaction had 971 of 1085 rows into moments by the time the
question was asked. The jump board (D62) survives compaction but is a top-5, not a series.

So the series is kept where it is produced. The node also records what never travels on the
wire — `raw_sigma`, `mean_px`, `autocorr`, `zero_diff` — which is the difference between "the
light moved", "this sensor is dispersing" and "neither".

The latch is a bug found while answering that question: a parameterless `POST /expose` sent to
slave2 mid-session was **accepted**, because the gap between two runs is 0,8 s and `g_measuring`
is false throughout it. That request rewrote the exposure to its own value (harmless) and called
`camera_stats_reset()` — the running block's camera statistics restarted mid-block. The same
request with `?exp=` would have moved the operating point under a session that had already
certified a rung, and nothing would have recorded it. The slave's own HTML page claimed "409
while measuring", which an operator reads as "409 during a session"; it now says what it does.

⚠ The latch has a known hole and it is deliberate: `PHASE_POOL_CONFIRM` parks up to 15 min
with a session genuinely open, so the latch expires under it. The alternative — no idle
release — would leave every slave locked after a master crash with no way back but a reboot,
which is worse.
Runs are 2..5 s and a sweep is ~10 s, so the latch never expires under a pass that is running.

⚠ `/camlog` is a RING. At ~2,9 s per window, 512 entries is ~25 minutes. Pull it inside the
session, per node; there is no master-side collector (`/diagjson?all=1` is 409 while measuring
and would not fit this payload anyway).

⚠ `t_ms` is each node's own uptime. Align nodes by `tag` or by ordinal, never by subtracting
timestamps across boards.

**Verifiziert** on hardware: all four nodes flashed (slaves `833d1d3f88b89786`, master
`d61419b5b5759ce8`), `/camlog` empty after boot and filling at one entry per window on all four
during a live session; `/expose`, `/linearity` and `/camtest` all answering 409 on slave2 while
that session ran. `/linearity` run on all three slaves with no session: ratios 1,75/2,06/2,07
(slave2), 1,57/1,92/2,03 (slave0), 1,90/2,00/1,94 (slave1) with `raw_sigma` 1,00..1,26 — steady
light on all three at that moment, which is consistent with the disturbance being episodic and
is exactly the reading the test exists to produce.

### D65 — LSB-as-is; one stream; ranking is z + concordance (2026-08-31)
**Entscheidung:** LSB bits are measured as-is. Extraction, z, archive, pairwise, pass health and
ranking all describe the same LSB stream. Concordance stays (half-window, leave-one-out).
`?wpre=` is the concordance weight against z (API default 0 = z alone, form 0,8). Wire
`Z:<z>,<h1>,<h2>[,wsig=]`. Soft-down trips on σ > 1,35 × this block's peer-median σ, not on
absolute 1,25. Sweep drops the `|σ−1|≤0,05` gate; autocorr bar 0,03; selection remains
LSB `|raw_bias−0,5|` plus relative raw-σ. Segment is 224 bits; `EL_SEG_MAX` 400000;
`RUN_SEGS_REF` 141026 (predicted 2× word rate, same wall time — re-measure). Slave `'B'` refused.

**Warum:** ranking already rode z_pre and conc (`wpre=0,8`). Adjacent-pixel XOR was leftover scaffolding
for a σ≈1 null that Bonferroni no longer tested. Dual channels were the overload.

**⚠ D17 stands as evidence, not as the rule.** LSB-as-is σ is not 1; centring takes the mean, not
the scale. Pass health is the measured σ. Do not pool with prior-instrument sessions (D1).

**⚠ `RUN_SEGS_REF` is predicted**, not live-calibrated. Read `focus_win_ms` after the first
dozen runs and correct the pair if the window is not the requested `?run=`.

### D66 — Always unattended; HTML still shows the current item (2026-08-31)
**Entscheidung:** attended mode is gone. No `PHASE_READY`, no `POST /ready`, no `?focus=`,
no form checkbox. The session is unattended. Scoring numbers and pass combinations still
appear on the HTML "Now:" card via `GET /focus` (always published, not gated on `focus_mode`).
Pool confirmation stays on `confirm=1` (UI only). CSV writes `focus=off`. `?focus=` → 400.

**Warum:** the operator is not part of the measurement. The card is a live readout, not a
GCP/PEAR observer protocol.

**Pooling:** new sessions pool with old `focus=off` only, never with `focus=on` (D1). D6 stands
as evidence, not as the rule.

### D67 — Rounds until Abort is the only session (2026-08-31)
**Entscheidung:** no single-pass. Every session is rounds: score → measure a cap-sized pool →
score again, until Abort. UI has no Unlimited checkbox; **Runs per round** is always on the
form. `unlimited` defaults on; `?unlimited=0` → 400. No pool-confirmation overlay; `/pool` 400.
`confirm=1` still writes NVS form prefs only.

**Warum:** the operator only ever ran rounds. The checkbox was a second experiment that is no
longer started.

**Pooling:** new sessions are `unlimited=on`. Do not pool with old single-pass (`unlimited=off`)
without splitting on `round` (D1).

### D68 — Ranking key in units of block σ (2026-08-31)
**Entscheidung:** `rank_key()` divides z and concordance by **that item's own block σ**, frozen
at `close_block()` (and on abort of a centred open block). Z* in the UI **is** the key. Session
`rank_sig_p` / `rank_sig_c` were diagnostics that did not scale the tables; D71 deleted
`rank_mean` / `rank_sigma` and D72 deleted these two as well. Scoring self-standardises on its own span; per-node centring of that span is D69.

**Warum:** a sweep can put the cameras on a different rung. Session-σ then mixes two instruments
into one Top-5: a loud new block inflates the denominator and crushes older extremes; at
`wpre>0` it can reorder two already-measured items whose Z/Conc never moved. Blocks exist to
stop that. Sweeps stay — without them the bits stop being bits. `/loops` carries
`rank_sig_p` / `rank_sig_c` per row so the scale is auditable. — **that publication was removed by D72**; the scale lives only in `s_bsig[]` at run time.

**Pooling:** tables post-D68 do not pool with pre-D68 tables. `z_raw` / `z_ctr` still do.

### D69 — Scoring centres per node, then concordance, like the pass (2026-08-31)
**Entscheidung:** `score_build_keys()` keeps per-node z / half-window z for the scoring span,
subtracts each node's own mean over the numbers it answered, Stouffer-recombines over the same
`have_mask`, then `conc_stouffer` on the **centred** halves. Mix and σ-scale unchanged (this
span is the block). `score_one_run` no longer throws the per-node values away.

**Warum:** on combined raw z, `(Z − mean_Z)` equals per-node centring only when every number
has the same k. Concordance was worse: it dropped the loudest **raw** node — the camera sitting
on the bright rung (offsets 20..95σ) — so that node never voted on the pool, even with a real
number-to-number jump. After centring it is often the quietest. The pass already did centre-then-drop;
scoring did drop-then-centre. Same key in the comments, different machine.

**Pooling:** pools chosen post-D69 do not pool with pre-D69 pools (D48 split still applies).
Pass `z_ctr` is unchanged.

### D70 — Node agreement as a column beside Z* (2026-09-01)
**Entscheidung:** `RunResult.node_sd` — the sample σ across the contributing nodes of their
block-centred z, each node first divided by **its own σ over that block** — is computed in
`center_block()` and shown as **Δn** in Top-5 / Bottom-5. NaN (UI: —) until the block is
centred and whenever fewer than two nodes have a block σ (k < 2, or a node with < 3 runs in
the block). It **ranks, selects and excludes nothing**; `rank_key()` is untouched.

**Warum:** Z* alone cannot say whether the four cameras moved together on an item or whether
one node carried the combine on its own — and with `NODE_SOFT_MIN_COMBINE` 1 a solo combine is
legal, so the k column does not answer it either. Per-node standardisation is what makes the
number readable: the per-node LSB σ is not 1 and differs between nodes (D17, D65), so the raw
spread of z_i would mostly report which nodes contributed. After the division the null is ≈ 1
for independent nodes at any scale — the same bargain D68 makes with the block σ. The mean the
spread is taken around **is** the combined z: Σz_i/√k = √k·mean, so "deviation from the
combined Z*" and "deviation from each other" are one number.

**Warum kein Rankingkanal:** small Δn is agreement, not evidence — a block-wide common mode
would produce it too, and centring has already removed exactly that. Making it a key would also
break the pre-registration: the key is fixed at D65/D68.

**Pooling:** no split. Display only; `z_raw` / `z_ctr` / `key` unchanged, and Δn is recomputable
offline from the per-node `z0..z3` in the CSV.

### D71 — `rank_mean` / `rank_sigma` deleted (2026-09-02)
**Entscheidung:** Both fields are gone — from `ElottoStatus`, from `/status`, from the CSV header,
from the UI's `st` object, and with them the `s_drop_ksum`/`ksumsq`/`kn` compaction seeds and the
`mean`/`sigma` parameters of `csv_row()`. (`rank_sig_p` / `rank_sig_c` followed one step later,
in D72.)

**Warum:** since D68 nothing reads them. Z* **is** `rank_key()`, standardised on the item's own
block σ, so the table never needed a session moment to scale against; `renderRunTable` took
`st.m`/`st.s` and used neither. What was left was a pair of numbers in the CSV header that only
a human could misread — and would: computed over `results[]`, which after the first compaction
holds the 100 most extreme items by |key| plus the current round, they read mean 0,55 / σ 3,29 on
a session whose `pass_sigma` was 1,027 (measured 2026-09-02, 27 517 items, 107 rows held). The
seeds fixed the arithmetic but not the question — a session moment of an already-standardised key
answers nothing. Deleting beats publishing a number nobody may use.

**Pooling:** no split. Nothing measured, ranked or archived changes. ⚠ A parser that reads the CSV
header positionally must drop the two `rank_*` fields (D72 drops three more).

### D72 — Session channel σ and the clamp counter deleted (2026-09-02)
**Entscheidung:** `rank_sig_p` / `rank_sig_c` are gone in both of their meanings — the session-wide
pair in `ElottoStatus` (CSV `pre_sig=` / `conc_sig=`, `/status`) and the per-block copy in
`LoopStat` (`/loops`). `pre_clamped` / `pre_clamp=` goes with them, and so do the compaction
moments `s_drop_psum`/`psumsq`/`csum`/`csumsq`/`cn` (only `s_drop_pn` survives, for `pre_n`).
`pre_n` and `pre_w` stay.

**Was bleibt:** `s_bsig[block].sig_p` / `.sig_c` — the per-block σ that `rank_key()` actually
divides by. That is unchanged and still frozen at `close_block()`. Only its publication is gone.

**Warum:** the session pair had exactly one consumer left after D68: it was the fallback divisor
for counting `pre_clamped`, for items whose block had no σ yet. That fallback contradicted
`rank_key()`, which deliberately has none — no block σ means the key is 0. So the counter could
report an item pinned at the clamp whose published Z* was 0. Since D68 the pair scaled nothing
else; `renderRunTable` took `st.sp`/`st.sc` and never read them. Two published numbers that only
one bookkeeping counter used, and that counter disagreed with the ranking, are worth less than the
questions they invite. The operator's call: remove, and give up `pre_clamped`.

**Was verloren geht, bewusst:** `/loops` no longer names the divisor of each block's Z*, so a
finished session's key is no longer reconstructible from the CSV alone — `z_ctr` and `zc_ctr` are
still there, but their block σ is not.

**Pooling:** no split. Nothing measured, ranked or archived changes. ⚠ A positional CSV-header
parser must drop `pre_clamp=`, `pre_sig=` and `conc_sig=`; `fw=` now follows `pre_n=` directly.

### D73 — The second LSB channel and the pool gate deleted (2026-09-02)
**Entscheidung:** two dead structures removed from the firmware, not merely marked dead.

**1. `zp_ctr` and its per-node archive.** D45 built a second channel from the un-XORed LSB
stream. D65 removed the adjacent-pixel XOR, at which point that channel WAS the z: `zp_ctr` was
assigned `(float)z` verbatim, `s_node_p` was fed the same `w.znode` array as `s_node_z`, and
`s_pacc` the same array as `s_nacc` — so its block centring reproduced z_ctr's arithmetic on
identical inputs. Gone: the `RunResult` field, ~128 KB of PSRAM archive, one `NodeAcc[MAX_NODES]`,
`node_p_store()`, `pacc_add_run()`, the `out_p` parameter of `results_row_z()` and the `"zp"` key
in the per-row `/status` JSON (which no UI code read).
⚠ The `?all=1` CSV is UNCHANGED: it never emitted `zp_ctr` or `p0..p3` — only a comment claimed
it did. Recover the value offline as `z_ctr`; pre-D65 archives keep their own column and their own
meaning.
⚠ `s_bsig[].sig_p` keeps its name and is unaffected — it was always computed from `rank_z()`
(= z_ctr), never from `zp_ctr`.

**2. `pre_n` redefined.** It counted `zp_ctr != 0`, i.e. every valid item, which made the UI's
"with pre n/valid ⚠" incapable of firing for a real reason. It now counts `zc_ctr != 0` — items
the CONCORDANCE term could rank; 0 means the leave-one-out drop left k < 2 and only the z term
carried that item. The compaction seed `s_drop_pn` follows.
⚠ Same field name, different quantity, from 2026-09-02. Never compare `pre_n` across that date.

**3. The pool-confirmation gate.** D66 stopped arming it and D67 made `POST /pool` answer 400, but
`PHASE_POOL_CONFIRM`, `pool_confirm` and the `"poolconfirm"` phase string stayed — and their
comments still described a session that STOPS after scoring. Deleted. `confirm=1` is now a local
`from_form` flag in the `/start` handler whose only effect is `prefs_save()`; `pool_auto` stays in
`/status` at a constant 1 as the record that nobody confirmed the pool.
⚠ `/status` no longer carries `pool_confirm`, and `phase` can no longer read `"poolconfirm"`.

**Pooling:** no split. No measured value moves; `z_raw`, `z_ctr`, `zc_ctr` and `key` are
bit-identical.

### D74 — Analog gain is NOT a calibration axis: measured on all four nodes, rejected (2026-09-02)
⛔ **`CONFIG_ELOTTO_CAM_REG_GAIN` stays 1023 and the sweep stays a pure exposure ladder.** Do not
re-open without new evidence — the measurement below is a full four-node ladder, not an argument.

**Why it was asked.** The bit stream is `LSB(b−a) == LSB(a) ^ LSB(b)` (extract.h), so the width of
the DIFFERENCE distribution cannot reach the LSB at all: the stream bias is `−2·e²` in the
per-frame LSB bias `e`, and `e` is set by how the single-frame histogram sits on the ADC codes.
Analog gain (0x350A/0x350B, pre-ADC) widens that histogram at constant photon count, so it is a
second knob on the same axis exposure already moves — and unlike exposure it separates "where in
the code range" from "how many photons".

**Method.** Per gain value: `POST /expose?gain=` on the node, then one session start so the opening
sweep walks the whole 4..512 exposure ladder at that gain, abort, read `/calibrate`. Sweeps hit all
four nodes at once, so the slaves were laddered together while the master held 1023 as a drift
control. Best rung per node, `|raw_bias−0,5|`:

| gain | master | slave0 | slave1 | slave2 |
|---|---|---|---|---|
| 1023 | 3,23e-3 | 3,48e-3 | **1,67e-3** | 2,91e-3 |
| 512 | 2,60e-3 | 4,70e-3 | 2,63e-3 | 3,46e-3 |
| 256 | 1,71e-3 | **1,87e-3** | 2,77e-3 | **2,19e-3** |
| 128 | **1,07e-3** | 2,88e-3 | 4,61e-3 | 5,61e-3 |
| 64 | 9,23e-3 | 6,27e-3 | 9,55e-3 | 7,95e-3 |

Repeatability at one setting, from five master repeats at gain 1023 over ~10 min: −3,04 / −3,88 /
−3,94 / −4,06 / −3,48e-3, i.e. ±15 %. An out-of-order master retest reproduced gain 128 at
−9,10e-4 against −1,07e-3, so the master's own factor of 3 is real.

**Why it is rejected anyway — three reasons, any one sufficient:**

1. **It does not reproduce across nodes.** The master improves 3×, slave0 1,9× and slave2 1,3× —
   but slave1, the brightest node, gets 1,6× WORSE, and its gain-1023 figure is the best number any
   node produced in the whole experiment. Three different nodes put their optimum at three
   different gains. That is node-to-node variation being reshuffled, not a gain law.
2. **The band does not move.** Best-case across the array is 1,7–3,5e-3 at gain 1023 and
   1,1–5,6e-3 at gain 128. No node reaches a different regime; the array's floor is unchanged.
3. **It buys the quantity the pipeline already subtracts.** A per-node bias of 3,5e-3 is a z offset
   of `(b−0,5)·28,28·√nseg` = 16 at the 26087 segments of a `run=0,5` session — and `center_block()`
   removes exactly that, whatever its size (D8, D65). What ranks and what soft-down trips on is the
   DISPERSION, and `raw_sigma` at the best rung is 0,98–1,18 across every node at every gain. It
   does not respond to gain at all.

**And the price would have been high.** At gain 256 and below the sweep certifies NOTHING on any
node — every rung fails `CAL_MAX_ZERO_DIFF` 0,125, because lower gain narrows the difference
distribution and more differences land on 0. Operating there needs D18's gate moved for all four
nodes, plus a rebuild and OTA of the whole array, plus a D1 archive split.

**One genuine finding kept from this, against D18's stated mechanism.** D18 justifies the zdiff gate
with "a zero difference has a deterministic LSB", which predicts a stream bias of about
`−zero_diff/2`. Measured at gain 128 / exp 512: zero_diff 0,229 would predict −0,115, and the node
read −1,07e-3 — a factor of 100 out. The zero bin is not a frozen population, it is the centre of
the even class and cancels against ±2, ±4. The GATE still binds correctly along the exposure axis
where its evidence was taken; the MECHANISM sentence is wrong. Also worth recording: the rungs D18
was built to reject (gain 1023, exp 4/8/16: mean_px 2,87 / 3,33 / 4,32) all fail
`CAL_MIN_MEAN_PX` 5,0 on their own today, so zdiff is not what is holding the dark end.

⚠ The exposure ladder tops out at 512, and every low-gain optimum sat ON that top rung. The measured
`mbit_s` is flat at 7,28–7,37 across the entire matrix, so exposure costs no rate here and the cap's
stated frame-time justification is not visible in the data. Untested, and out of scope for D74.

**Pooling:** no split. Every node was restored to gain 1023 and re-certified; no session data was
taken at any other gain.

### D75 — The ranking key: unbounded, per-item channel weights (2026-09-02)
Two properties of `rank_key()` (the pass) and `score_build_keys()` (the pool), which must agree.

**1. Unbounded.** Nothing truncates an extreme item. An extreme item is what the instrument exists
to find, and there is no explosion to guard against: the item sits INSIDE the σ it divides by, so

    |key| <= (n-1)/√n,  n = items contributing to that σ

whatever σ comes out. A quiet block cannot manufacture a large key. Worked: n = 208, the block
length of a 300-runs-per-round session, gives 207/√208 = 14,4. A scoring span holds at most 50
numbers, so its ceiling is 49/√50 = 6,9. Pure chance over 38000 items reaches about 4.

⚠ **The ceiling moves with n, so Z\* does not compare across blocks of different length** — 11,1 at
n = 126 (`?run=5`, a full 15-minute block), 14,4 at 208, 23,1 at 535 (`?run=0,5`). n follows the
measuring window and the round boundary, both operator-set, and a block ends at whichever of the two
comes first. Read n off the CSV as ranked rows ÷ blocks; it cannot be derived from `?run=`, because
the delivered window is set by the slowest node's bit rate (0,5 s asked, 1,17 s delivered).

**2. A channel that cannot rank an item loses its WEIGHT, not just its value.** Both keys mix z and
concordance and divide by `√((1−p)² + p²)`, p = `?wpre=`. A one-channel value under a two-channel
normaliser comes out small — at the form's 0,8 it is 0,24 of proper size — and then competes in the
same tables against fully scaled items. So the normaliser is rebuilt per item from the channels that
item actually has: z alone is ranked on z at full scale, concordance alone likewise, neither is 0.

Absent means exactly: `zc_ctr == 0` (the halves disagreed, or fewer than two nodes survived the
loudest-node drop — this is what `pre_n` counts), or `sig_c == 0` for the whole block. Both are
normal states, not faults.

⚠ **In scoring the condition is PER NUMBER**, and it decides the pool: `zcc[k]` is NaN for any
number whose halves disagreed, so those numbers are the ones the weighting has to get right or they
lose the pool to numbers that happened to carry a concordance value.

⛔ This is not a σ fallback. Each surviving channel still divides by its OWN block σ; no scale is
borrowed from anywhere.

**Pooling:** ⚠ **splits the tables AND the chosen pool.** `z_raw` / `z_ctr` / `zc_ctr` are untouched
and pool across the boundary.

### D76 — One block is one round; the wall-clock sweep trigger is gone (2026-09-02)
⛔ **The round boundary is the only block boundary, and the only sweep trigger.** `?calint=` answers
400. `?cal=<ms>` remains the sweep budget and `?cal=0` the no-calibration control.

**Why.** `rank_key()` divides an item by the σ of its own block, and because the item is inside that
σ the largest value a block can produce is `(n−1)/√n` in that block's item count n. A wall-clock
trigger made n depend on where the 15-minute mark happened to fall inside a round, so **two blocks
of ONE session had different ceilings and their Z\* did not compare** — 11,1 at n = 126, 14,4 at
208, 23,1 at 535. Ranking within a session has to be comparable; that is the whole point of it.

Round = block fixes n, because every round measures the same `maxruns`-sized space. `?maxruns=` is
therefore the block-length knob, and it sets the drift regression's resolution with it.
⚠ The last round is cut short by Abort, so its block is the one short one in a session.
⚠ Across sessions with different `?maxruns=` the ceiling still differs — a pooling question.

**What replaces the interval as a guard.** Nothing automatic. The start form previews the round
length per mode and warns above `ROUND_WARN_MS` (30 min), and that is all it does: a long round is
the operator's call. The warning names the cost — every item in the round is centred on one mean and
scaled by one σ, and the cameras are re-swept only at the boundary.

`calibrate_all()` keeps one backstop, now floored at **twice the sweep budget** rather than at the
operator's interval: a round with a very small `?maxruns=` can be shorter than the sweep it would
trigger, and re-tuning a camera that was tuned 20 s ago measures the sweep's own noise. At any sane
round length it never fires; when it does, the block still closes on time, only the sweep is skipped.

**Pooling:** no split for the archive. `z_raw` / `z_ctr` / `zc_ctr` are untouched. ⚠ A positional
`/status` parser must drop `cal_interval_ms`.
