# 2026-08-18 · 6-of-49 · unlimited · run=1 s — node-health diagnostic

Short loaded run, started to answer "does slave1's fault follow the camera?"
Aborted deliberately after 5 blocks. **A diagnostic, not a result run.**

`run_s=1,00 run_segs=26087 gap_s=0,50` · unlimited, `maxruns=100` (9 numbers,
84 combinations per round) · `calint` 5 min · unattended · 407 items, 4 closed
blocks plus a partial.

⚠ **A THIRD instrument.** 26.087 segments per item, against 130.435 for the 5 s
post-08-18 sessions and 70.513 pre-08-18. Do not pool it with either.
⚠ `gap_s` is 0,50 and not the documented 40 % of `run`: the auto-gap has an
undocumented 500 ms floor (`elotto.c`), which binds at `run=1`.

## What it found

**Block-mean over-dispersion, on ALL FOUR nodes.** Standardising each node's
block mean by its own in-block sd, **10 of 20 node-blocks exceed |z| = 2**,
where 0,9 are expected. Lag-1 autocorrelation within blocks is ~0 (mean −0,026
against SE 0,109), so this is not an artefact of treating items as independent.
The zero point wanders by ~0,25 in z units between blocks where sampling noise
alone allows 0,11 — a factor of ~2,3.

⚠ **This does not look like "two bad arms."** slave0, never a suspect, is as
scattered as slave1. Block centring absorbs all of it — `pass_sigma` 1,014 — so
what the published statistics lose to it is any real effect that is CONSTANT
across a block, and the thing being removed is large.

**The master reproduced its excursion**: block 3 mean −1,712 (−14,9 SE) at
in-block sd 1,051 — noise fine, zero point moved, the exact 08-13 signature.
Soft-down fired (`|mean| > NODE_MEAN_SOFT 1,50`) and the block was quarantined
(`ranked=323 excl=84`). Both mechanisms exercised on a real event for the first
time, and both behaved.

**The camera statistics did not see it.** In that same block the master's
`cam_bias` was 0,500003 — the best reading in the run — with `cam_cal=1` and no
stalls. The largest `cam_bias` deviation in the run (slave0, +2,56e-4) produced
a nearly clean block. ⚠ **A clean `/diag` is not evidence of a clean arm.**

**Baseline foreshadowing, one instance.** Block 3's `base` — the master's own
baseline mean, ten runs taken BEFORE the block — read −1,802 against a block
mean of −1,712, so the offset was fully present before the first item was
measured. Blocks 2 and 4 disagree by ~1,5 SE. A lead, not a relationship.

## Files
- `results_all.csv` — every measured item (`?all=1`; German CSV, `;` and `,`)
- `loops.json` — per-block per-node offsets, sigmas, chosen exposure, cam health
- `status_final.json` — the node mapping

⚠ `z0..z3` are **DISCOVERY order**, which is not slave order and changes between
sessions. For this run: **0 = master, 1 = slave1 (.145), 2 = slave2 (.155),
3 = slave0 (.103)**, per the CSV header's `# nodes=` line.
