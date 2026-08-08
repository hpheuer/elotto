# 2026-08-08 — 6-of-49, pool 11, complete pass (462/462)

Attended (`focus=on`), `run=5 s` / `gap=2 s` (measured window 6.77 s, gap 2.01 s),
`calint=15 min` → 5 blocks, `cal_budget_ms=10000`. Wall time 5 746 935 ms ≈ 1 h 36 min.
No pauses, `net_lost=0`, `net_stale=0`, no stalls, no reboots, all four nodes for the whole pass,
`cam_cal=1` on every node in every block.

**Firmware: commit `f04c2b8`.** `status.json` reads `fw_version 673a86d-dirty` because the image
was built and flashed before the commit was made; the source is byte-identical to `f04c2b8`.
This is the first pass measured with the nearest-zero group and the German CSV.

## Files

| file | what |
|---|---|
| `results.csv` | the record — all 462 items, raw z, measurement order (`?all=1`) |
| `summary.csv` | the 15-row published summary (high/low/zero, with `z_std`) |
| `status.json` | end-of-pass `/status` |
| `loops.json` | per-block health, 5 blocks |
| `ladder_1xx.json` | `/calibrate` per node, last sweep |

## What it says

**Within a block the array is ideal; between blocks it is not.**

| | |
|---|---|
| pass σ | **1.3635** |
| within-block pooled σ | **1.0096** |
| block means | −0.384 · −1.261 · −0.459 · **+1.391** · +0.205 |
| worst pairwise \|r\| | 0.038 (\|r\|√n = 0.82, flag at 3) |

The whole σ excess is the block-to-block offset spread — 2.65 z of range across five blocks —
not inter-node correlation and not per-node σ. That is a **much better instrument than the
2026-08-05 pass** (σ 2.19 with `.155` at 1.494 and `.103` at 1.231 individually); the difference
is that the sweep certified every rung this time, where 08-05 ran with `cam_cal=0` throughout.

**Block 3's excursion is one node.** In that block `.145` (slave1, the known weak node) sat at
mean **+3.11 with σ 1.560** while the other three stayed at −0.43 / +0.30 / −0.19 and σ ≈ 0.9–1.1.
That single node-block produced **4 of the 5 top items** — the block-clustering signature again,
and this time attributable to a specific node in a specific block.

**Re-centred per block, the pass is null.** Max \|z\| over the 462 items becomes **3.62** against
the **3.50** expected as the maximum of 462 standard normal draws. Only item 459 (block 0, within-block
z +3.62) appears in both the raw and the re-centred top-5; the other four are block-3 artifacts.

## Two defects this pass exposed

1. **`drift_slope` / `drift_t` reported exactly 0.00000 / 0.00** while the master's per-block mean
   (`raw_m`) rose monotonically −2.635 → −2.390 → −0.503 → −0.433 → −0.385, i.e. +2.25 z over five
   blocks. The regression should have produced a large positive slope. Drift is specified as the
   first diagnostic to read on a raw-z pass, and here it silently read zero.
2. **The published ranking is against the pass mean, which mixes blocks.** `z_std` in `summary.csv`
   is (z − pass mean)/pass σ; with block offsets spanning 2.65 z that statistic charges an item for
   the block it happened to land in. Within-block centring is the honest comparison and it changes
   four of the five top items.
