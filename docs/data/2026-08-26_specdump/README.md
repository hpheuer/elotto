# 2026-08-26 — where the spectral structure actually is

**The row-frequency hypothesis is REFUTED. The structure is low-frequency DRIFT.**

`GET /specdump?segs=32768` (new, all four nodes): the mean Welch periodogram of a real
measurement window off the node's own camera, 511 normalised bins, 32 windows.

| condition | sd/mean | bin 1 | bins 1-20 | bin 256 |
|---|---|---|---|---|
| slave0 fold=1 exp64  | 0,1754 | 1,07x | 4,06 % | 0,94x |
| slave0 fold=0 exp64  | 0,1752 | 1,14x | 4,27 % | 0,81x |
| slave0 fold=1 exp128 | 0,6904 | **8,87x** | 8,19 % | 0,59x |
| slave0 fold=0 exp128 | 1,1396 | **23,81x** | 9,15 % | 0,79x |
| flat expectation (M=32) | 0,1768 | 1,00x | 3,91 % | 1,00x |

## What was predicted, and what happened

Predicted (2026-08-26, from raster geometry at RAW8 800x800): with the fold off the row rate
is 4,0 segments per row and should put a line on **bin 256**; with the fold on it is 2,0
segments per row and lands on the discarded Nyquist bin.

**There is no line at bin 256 in any of the four conditions** — it reads 0,59x to 0,94x, i.e.
below the mean. The arithmetic was right; the PREMISE was wrong. There is no spatially
periodic structure in the LSB stream for the row rate to alias.

What there is instead is a monotone excess at the LOWEST resolvable frequencies: bin 1 rises
from 1,07x to 23,81x as the source degrades, and bins 1-20 from 4,06 % to 9,15 %. Bin 1 is a
period of 1024 segments, i.e. the longest thing the window can see — that is **drift inside the
window**, not a line. This is the direct spectral confirmation of D17's finding that the
unfolded LSB bias is non-stationary.

⚠ **Therefore a stride/shuffle fold is pointless** and the "spatial shuffling" option is dead
with it: both attack spatial structure, and there is none. That closes the question that
option (b) of 2026-08-26 was raised to answer.

⚠ **The MIPI/CSI clock was never a candidate** and this does not change that: extraction reads
a completed frame out of PSRAM in raster order, not in step with the lane, so a timing clock
cannot produce a fixed segment period at all. Only spatial periods can, and there are none.

## The bigger finding: EXPOSURE is the variable, not the fold

At exp 64 both arms are flat and clean (sd/mean 0,175 against 0,177 ideal). At exp 128 both are
disturbed. The fold SUPPRESSES the drift but does not remove it — at exp 128 it takes bin 1
from 23,81x to 8,87x and the emitted sigma from 1,8359 to 1,4233, still over
`NODE_SIGMA_SOFT` 1,25.

⚠ **exp 128 is a rung slave0's own sweep never selects.** In the 7354-item session it used
16/32/64 only. This was forced by hand with `POST /expose`; the gates already reject it. The
run demonstrates the gates being right, not a live fault.

So the fold's real job is now correctly named: it squares a **drifting bias**, which is a
TEMPORAL defect. That is why squaring is the right minimal operation and why nothing spatial
would have helped.

## The pre-fold monitor earned itself in the same run

`raw_bias` / `raw_sigma` in `/diag` (new): at exp 128 slave0 reads raw_sigma **1,7680** against
an emitted sigma of 1,4233 — the front end is degraded and the fold is masking part of it.
Before this build, seeing that number required building and flashing a fold-off image.
⚠ With the fold OFF the two pairs must agree exactly, and they do: 0,491186 / 1,8359 on both
sides. That is the built-in consistency check from `cam_raw_t`.

Cost: `/camtest` `ms_pair_ext` **39,3 ms** against the historical 39,5-39,8 with the monitor
running in the live path — no measurable cost. `equal:true`, 6 cases, popcount ok.

## Files
`master_foldon.csv` (exp16) · `slave0_foldon.csv` / `slave0_foldoff.csv` (exp64) ·
`slave0_foldon_exp128.csv` / `slave0_foldoff_exp128.csv`.
Header carries fold, exposure, frame geometry, seg_per_row, pred_bin, and both stat pairs.
