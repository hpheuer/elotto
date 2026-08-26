# 2026-08-26 — fold-off re-test (D17 re-check)

**Verdict: D17 holds.** Fold-off still fails, now visible in three independent places.

Method: `CONFIG_ELOTTO_CAM_XOR_FOLD=n` built into the slave image and OTA'd to **slave0
(192.168.178.103)** only. Master + slave2 + slave1 stayed folded as the within-session control.
6-of-49, `run=1.5 s`, `gap=0.6`, `went=0.6`, `cal=10000`, `calint=240000`, `baseline=3`,
unattended. 09:28–09:51, 346 items, 4 blocks, aborted on purpose.

Discovery order in the CSV: `master, 155, 145, 103` — **z3/h3 is the fold-off node.**
fw_nodes: master `5cba7061d053abab`, slaves `a0a5d77e03fcb942`, slave0 `37175acddd1aa225` (fold off).

## 1. Calibration: 0 of 9 rungs certify

`ok:false, chosen:-1`. **Every** rung fails `CAM_CAL_FAIL_BIAS`(0x04) **and**
`CAM_CAL_FAIL_SIGMA`(0x10). Folded slave1 certified 4 of 9 in the same minute.

| exp | fold-off bias / sigma / code | fold-on bias / sigma / code |
|---|---|---|
| 32  | 0.494168 / 1.0763 / 20  | 0.500187 / 1.0014 / 0 OK |
| 64  | 0.497738 / 1.0698 / 20  | 0.499674 / 1.0063 / 0 OK |
| 128 | 0.493375 / 1.7233 / 28  | 0.499942 / 1.0080 / 0 OK |
| 256 | 0.486681 / 3.2656 / 28  | 0.493699 / 2.1138 / 28 |
| 512 | 0.473097 / 6.1809 / 28  | 0.455229 / 4.8011 / 284 |

Codes: 20=SIGMA+BIAS, 28=+AUTOC, 148=+DARK, 404=+ZDIFF.

## 2. Per-block z sigma — soft-down trips in block 3

| blk | master | slave2 | slave1 | slave0 (fold off) |
|---|---|---|---|---|
| 1 | -0.122/0.886 | +1.160/1.018 | -0.358/0.938 | -0.610/**1.176** |
| 2 | +0.150/1.036 | -0.193/0.941 | -0.511/1.050 | -1.069/1.033 |
| 3 | -0.522/1.014 | -0.324/0.937 | +1.062/0.997 | -0.132/**1.273 TRIP** |
| 4 | -0.102/1.101 | +0.151/1.018 | +0.110/1.032 | +0.598/1.058 soft |

Block 3 quarantined (`quar=1`, 73 items `skip_rank=1`). `k` fell to 3 for 126 of 346 items.

Session-wide raw z sd: master 0.9973, slave2 1.1179, slave1 1.1667, **slave0 1.2696**.
Milder than D17's 2.153 — shorter run (1.5 s vs 3 s) and a better-lit rung — but it still
trips `NODE_SIGMA_SOFT` within three blocks.

## 3. NEW: the entropy channel is the sharpest discriminator

Per-node raw z_h over 346 items:

| node | mean | sd |
|---|---|---|
| master  | +0.0790 | 0.9984 |
| slave2  | +0.0538 | 1.0208 |
| slave1  | -0.1382 | 1.0203 |
| **slave0 (fold off)** | **-201.7452** | 11.0246 |

Two hundred sigma off the null. The unfolded LSB stream carries gross per-frame spectral
structure that the fold cancels; `ENT_Z_CLAMP` fired on it constantly. This channel did not
exist in 2026-07-26 and is a far more sensitive fold detector than sigma is.

## Open, unresolved

`cam_bias` at fold-off (0.493490, i.e. -6.5e-3) converts via the D19 relation
`(b-0.5)*200*sqrt(nseg)/GCP_SEGMENT_SD` to **-36 z per run** at `run_segs=39130`. The node's
measured block mean was **-0.61**. gcp.c reads the same ring words the bias statistic counts,
so the two should agree; they differ by ~50x. Not chased down here. It means the bias gate at
fold-off rejects on a statistic that overstates the z impact — the gate verdict is still right,
but for a reason that is not fully understood.

## Restore

slave0 reflashed folded (`2ee9cecec55bc4d0` — same source as the other two, different build
timestamp, so a different ELF SHA) and set to exp 64. All three slaves verified at
bias ~0.4999, sigma ~1.000, 5.71 Mbit/s.
