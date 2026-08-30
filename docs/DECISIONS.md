# elotto — decision log

CLAUDE.md holds the RULES. This file holds the EVIDENCE behind them: what was measured, what was
tried and rejected, and what it cost. Split out on 2026-08-19 because CLAUDE.md is loaded into
context every session and had grown to 830 lines, more than half of it history.

Entries are append-only by number and cited from CLAUDE.md as `[D<n>]`. Bodies may be shortened —
full prose at `git show 1e62bca:docs/DECISIONS.md` (pre-2026-08-28-docs-trim). Working-tree
entries after that commit keep enough text that the decision survives. A rule and its entry must
be changed together — a rule whose evidence has moved on is worse than no rule.

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
⚠ Selection key moved to pre-fold bias in D46; the σ-margin rule above still holds.

### D17 — No fold trial: a 3 s window cannot certify fold-off
**Entscheidung:** XOR fold permanently on. A 3 s window certified fold-off (bias inside 1e-3) and
the node's run σ then blew out (1,043 → 2,153). Cause: unfolded LSB bias is non-stationary
(~2,1e-3 travel). Re-test 2026-08-26 (slave0 fold-off alone): sweep certified nothing, soft-down in
block 3, entropy z_h −201,7. Failure is SCALE not location — recentring cannot rescue fold-off.

**Zahlen:** Data `docs/data/2026-08-26_foldoff_trial_slave0/`. At rung 128 fold-off: bias 0,493375 /
σ 1,7233 vs folded 0,499942 / 1,0080.

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
**Entscheidung:** Idle rate 5,71 via `-O2` (3,81) + inline popcount (4,60) + word-wise extraction.
No Zbb; `__builtin_popcount` was a library call. `LSB(b−a) ≡ LSB(a)⊕LSB(b)`.

### D23 — The idle ceiling is the sensor, and we are at 98,5 % of it
The old "71 % of the ceiling" figure assumed 50 fps from the datasheet. `fps_raw`
(`camera_fps_probe()`, run by `/camtest` before the benchmark so it sees an idle CPU) times the
sensor with extraction stopped: **36,11 and 36,22 fps** on two runs → a pair every 55,2–55,4 ms
against a live 56,0 ms, i.e. 18,1 pairs/s × 320.000 bit = **5,80 Mbit/s** against 5,71 measured.

The measured PSRAM read floor is 7,1–7,9 cycles/pixel against ~21 for extraction+statistics.

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
`PASS_KEEP_PER_TABLE` (16) extremes per table, fold the rest into moments. Top/Bottom exact;
Nearest-zero effectively exact (centring pins block means at 0). Only when the next round would not
fit. `round_base` = results[] index; `round_item_base` = items_done — they diverge after compaction;
using the wrong one double-counted survivors on first hardware run (fixed 2026-08-20).
`runs_completed` = rows held; `items_done` = session count. `LOOP_HIST` 128 → 1024.

**Zahlen:** Replay 8000 → 48 rows, σ/χ² bit-identical. Verified: `compacted` 15828,
`pass_n_valid` == `completed` == 17766.

### D43 — The spectral structure is DRIFT, not a row line; the pre-fold monitor (2026-08-26)
**Entscheidung:** `/specdump` shows excess at the lowest bins (drift inside the window), not at the
predicted row-rate bin 256 — no spatial period in the LSB stream. Exposure is the variable; the fold
suppresses drift without removing it. Stride/shuffle folds and spatial shuffling are dead. Pre-fold
monitor (`raw_bias`/`raw_sigma`) published; `GET /diagjson?all=1` is the array collector (409 while
measuring). Folded selection was noise-limited — resolved in D46.

**Zahlen:** exp128 fold-off bin1 23,81× / fold-on 8,87×; at exp64 both flat. Data:
`docs/data/2026-08-26_specdump/`.

### D45 — The pre-fold z: the channel the fold throws away (2026-08-26)
**Entscheidung:** Fold maps bias ε → ~2ε² (√2·ε suppression). Fold stays for a stable null (D17);
raw stream scored/centred/archived as third channel. Ranks only, never p-values (σ 1,03–1,10).
Key `((1−w−p)·z_ctr − w·z_h + p·z_pre)/√…` with `?wpre=` default 0; z_pre PLUS. Uncentred pre-fold
ranks nodes (offsets to −47σ). `CAM_RAW_EVERY` gone; `NUM_RUNS` 8000→7200 (Euro 7920 needs one
compaction). `pre_w` splits the pooling table for TABLES.

**Zahlen:**
| ε | z_roh (1 s) | z_gefaltet | suppression |
|---|---|---|---|
| 1e-4 | 0,457 | 6,46e-5 | 7071× |

Wire `Z:<z>,<H>,<z_pre>`; CSV `zp_ctr;p0..p3`.

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
- NIST runs as a ranking channel (D54 tried, D55 deleted — underdispersed, orthogonal to Pre);
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
**Entscheidung:** Selection key is `|raw_bias−0,5|` (pre-fold); folded key was noise (exp16 beat 64
by 6e-6 ≈ 1/40 SE). Incumbent kept unless challenger beats by `CAL_KEEP_MARGIN_K` 3 SE. Absolute
pre-fold bars are dead ends (non-stationary per node; certified-empty a healthy node) — raw bias
selects never gates; raw σ is relative (`CAL_RAW_SIGMA_K` 1,35 × ladder best, one-sided). Runs
statistic armed only inside a sweep (permanently on: 5,38 vs 5,71 Mbit/s).

**Zahlen:** Master offset by rung over 50 blocks: exp16 −0,643 / 32 −0,321 / 64 −0,057.

### D47 — Four health mechanisms, one of them load-bearing; the other three are gone (2026-08-28)
**Entscheidung:** Deleted `null_flags`, `NB` attribution, per-node CUSUM. Soft-down + quarantine
alone affect data. Software publishes numbers, draws no verdict; `PAIR_FLAG_T`/`DRIFT_FLAG_T` are
display hints only. Soft-down still watches FOLDED σ — at `?wpre=` 0,8 ranking rides pre-fold;
deliberately not closed in software (answer: light). CSV lost `null_flags=`.

| mechanism | effect on the data |
|---|---|
| soft-down + quarantine | **real** |
| `null_flags` / `NB` / CUSUM | none (removed) |

### D48 — The baseline phase is gone; the scoring key is the pass key (2026-08-28)
**Entscheidung:** Baseline phase DELETED — block centring already gives the drift reference from
~200 runs. Scoring key now uses all three channels via `score_build_keys()`; scoring pass
centres/standardises itself per channel (required for pre-fold — without it every candidate hits
`ENT_Z_CLAMP`). `?baseline=` → 400. Slave still answers `B`. Pooling: split on 2026-08-28.

### D49 — The start form remembers its last values, the API remembers nothing (2026-08-28)
**Entscheidung:** Form fields persist in NVS, served as script on `/`. API defaults untouched. Only
`confirm=1` starts write (UI only) — curl must not overwrite operator weights with API defaults.
Mode not remembered. Fields: measuring time, focus, unlimited + runs per round, score
direction, pre-fold weight. Implemented 2026-08-29 (was documented, the write/read was
missing, and focus was the one the operator noticed).

### D50 — The block table records what the camera DID, not what the sweep found (2026-08-28)
**Entscheidung:** Sweep fields (`cam_bias` etc.) are identical across blocks on one setting — a
σ 6,94 block was unattributable. Added during-block `cam_sig` (folded σ) / `cam_rsig` (pre-fold σ) /
`cam_px` (mean pixel). `cam_rsig` recorded never gated. UI: separate `cam σ` (pre-fold) and `z σ`
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

### D52 — Bright-end and folded-bias gates are publish-only (2026-08-28)

**Entscheidung:** `CAM_CAL_FAIL_LIGHT` (`CAL_MAX_MEAN_PX` 100) and `CAM_CAL_FAIL_BIAS`
(folded `|bias−0,5|` vs `cal_bias_bar`) no longer reject a rung. Selection stays pre-fold
`|raw_bias−0,5|`; rejection stays folded σ / autocorr / dark / zero_diff / stuck / bits / RSIG.

**Warum:** LIGHT rejected the best measured rung (exp 64, mean_px 68,7) while saturation already
fails SIGMA `[D20]`. Folded bias is noise-limited after the fold and duplicates what the pre-fold
key already ranks `[D19]``[D46]`. Constants and `cal_bias_bar()` remain for `/calibrate` readability
and old CSV comparability; bit values stay defined as legacy.

### D53 — Spectral-entropy channel deleted (2026-08-28)
**Entscheidung:** FFT / spectral-entropy ranking channel removed (user). `gcp_spec.c` gone;
`?went=` → 400; no `ent_w` / `zh_ctr` / `h0..h3` / `/spectest` / `/specdump`. Ranking key is
`((1−p)·z_ctr + p·z_pre)/√…` with `?wpre=` only. Offline H re-rank not needed; a pre-fold segment
FFT was considered and not built.

### D54 — Pre-fold runs ranking channel (2026-08-28)
**Entscheidung:** NIST-style runs z on the pre-fold stream ranks with weight `?wruns=` (MINUS: fewer runs = clumpy = interesting). Key `((1-p-r)·z + p·zp* - r·zr*)/√…`. Form default 0,2; API 0. Runs counter always armed (bit-rate cost). Wire `Z:z,nan,z_pre,z_runs`. Tables show Pre and Runs beside Z*. Deleted the next day: `[D55]`.

### D55 — Runs ranking channel deleted (2026-08-29)
**Entscheidung:** `?wruns=` → 400. No `runs_w` / `zr_ctr` / `rank_sig_r`. Ranking key is again
`((1−p)·z_ctr + p·z_pre)/√…` with `?wpre=` only. Wire extra fields after `z_pre` ignored (old
slaves may still send a 4th). Runs counter armed **only inside a sweep** again (D46) — the
measurement-path cost is gone. Sweep still publishes `raw_runs_z` per rung, still not a gate.

**Warum:** Unlimited 6-of-49, 77 rounds, 16328 items, `pre_w=1`. Folded pass textbook H₀
(σ 0,996, v_eff 0,996, |r|≤0,011, drift_t 1,23). Pre overdispersed (`rank_sig_p` 1,94) with
outliers to −13,4. Runs underdispersed (`rank_sig_r` 0,72) and the −13,4 item had Runs ≈ 0.
NIST conditions on observed π, so a mean shift — the GCP alternative, and what Pre measures —
is invisible by construction. What Runs would see (clumping) is a camera property; block
centring subtracts it and leaves sampling noise. Same failure mode as the spectral channel
(D53).

### D56 — Half-window concordance ranking; nearest-zero and fat archive gone (2026-08-29)
**Entscheidung:** Ranking key at `wpre>0` is leave-one-out Stouffer of per-node half-window
pre-fold z, not combined zp_ctr. Per node: split at nseg/2; same sign → `√2·min(|h1|,|h2|)`;
else 0. Then drop the loudest node. Wire `Z:z,nan,z_pre,h1,h2`. zp_ctr still archived and
shown as Z-Pre; Conc is zc_ctr.

Nearest-zero table deleted. Unlimited rounds compact every boundary to the 100 most extreme
`|rank_key|` items (both tails). Pass moments stay exact.

**Warum:** Pre=−13 items sat in blocks where one node had a −2σ bright-rung offset (L26–34
slave0, L17–25/L66–72 slave2). Combined Pre ranks that node. Half-window kills a one-sided
glitch; leave-one-out kills a standing single-node offset. A stable 4-node bias survives
(z_hw ≈ z_full when halves agree). Lag-1 autocorr of ±1 is Runs (centred) or Pre (not);
not a third channel.

### D57 — Bonferroni line deleted (2026-08-29)
**Entscheidung:** No Bonferroni / corrected p / "consistent with chance" line. `p_corr` /
`best_z` gone from `/status`. `comparisons` stays as the valid-item count for the table
heading. Pass mean/σ/χ² stay — those are instrument health on the folded stream.

**Warum:** Bonferroni tests the XOR-folded z. The effect this experiment looks for is a
mean-bias that the fold suppresses by √2·ε. A p=1.000 on folded data does not say whether
a node is delivering usable pre-fold noise, and it cannot detect the signal. User.

### D58 — Both pre-fold channels rank, p split evenly (2026-08-29)
**Entscheidung:** The ranking key carries THREE terms, not two:

```
key = ((1−p)·z_ctr + (p/2)·zp_ctr/σ_p + (p/2)·zc_ctr/σ_c) / √((1−p)² + 2·(p/2)²)
```

`p = ?wpre=` still splits folded against pre-fold; the pre-fold half is then split evenly
between the combined pre-fold z (`zp_ctr`, D45) and the half-window concordance (`zc_ctr`,
D56). Each is standardised by **its own** measured σ — `rank_sig_p` and the new
`rank_sig_c`; the concordance takes `min()` of two halves and drops the loudest node, so its
σ is materially smaller and one shared σ would silently reweight the key. Both σ carry their
own compaction moments (`s_drop_c*`) and their own count: an item can hold `zp_ctr` and no
`zc_ctr` when k < 2 survived the leave-one-out drop.

`score_build_keys()` takes the same three channels, each standardised on the scoring pass's
own candidates (D48). `/status` publishes `rank_sig_c`; the CSV header gains `conc_sig=`
after `pre_sig=`. `pre_n` counts items carrying `zp_ctr` and is SEEDED with the compaction
moments, because the UI prints it against `pass_n_valid`, which is seeded too —
unseeded the ratio collapsed after the first compaction and the "with pre
n/valid ⚠" hint fired on its own bookkeeping (seen 2026-08-30 as "pre 132/788"
with every surviving row carrying a value). `pre_clamped` is not seeded and
cannot be: whether an item sits at the clamp depends on the current σ. `?wpre=0` still reproduces pure-z ranking
exactly — the control arm is unchanged, and the UI field stays.

**Warum:** User, after noticing `zp_ctr` had no weight in the key at all. D56 replaced the
combined pre-fold z with the concordance rather than adding to it, so from 2026-08-29 the
channel the project calls its lead channel — the one carrying the mean-bias the fold
suppresses by √2·ε — was archive-only. The two answer different questions and neither
subsumes the other: `zp_ctr` is the sensitive one and a single loud node can own it;
`zc_ctr` is robust against exactly that and pays sensitivity for it. An even split needs no
third free parameter and leaves no session ranked on a weight nobody chose.

**⚠ Known and accepted:** the √ normaliser assumes the three terms are independent, and
`zp_ctr`/`zc_ctr` come from the same bits — so at p → 1 the key's variance runs above 1. It
sets the printed scale only: one constant divisor reorders nothing, and Z* studentises on the
measured `rank_sigma`.

### D59 — Gain 256 tried and reverted; slave0's light was the real fix (2026-08-30)
**Entscheidung:** `CONFIG_ELOTTO_CAM_REG_GAIN` stays at **1023**. The operator's physical
dimming of slave0 stays and is the change that worked. `raw_words`/`mid_words` in
`gcp_zscore_pre()` are derived (`7*nseg`) instead of counted per word — bit-identical, no
pooling split. `pre_n` is seeded with the compaction moments like `pass_n_valid`.

**Warum das Licht:** slave0 sat at ~8x the illumination of master and slave2, so the sweep
could only reach exp 4/8/16 and certified 3 of 9 rungs at `raw_sigma` 1,36 — the worst node
in the array, and the one dragging the combined pre-fold σ. After dimming by ~6,7x it
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
word**, the same unit `mbit_per_sec` uses — the 64 pre-fold pixels behind a folded word are
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
**Entscheidung:** every node measures the folded per-mini-run sigma of **its own measurement
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

⚠ `WSIG_MIN_JUMP` 0,05 is a floor against filling the board on a short session, nothing
more: at the measured 0,0177 it is 2,8 sigma, i.e. ordinary scatter. **The x-sigma column is
what makes a row readable**, and it comes from `wsig_sd`, accumulated over every jump including
the quiet ones — taking it only from those clearing the floor would measure the floor.

⚠ Filled from the MEASURING pass only. A scoring run has no item identity to name.

**Verifiziert** on hardware, 21 items x 4 nodes: 80 jumps, noise 0,0177, exactly one above the
floor (node1, item 3802, -0,0512) — and exactly that one on the board, with matching
prev/now/jump. `w0..w3` populated on every row from all four nodes.
