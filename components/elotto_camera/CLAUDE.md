# elotto_camera — extraction rules

Loaded when working in this component. The project-wide rules are in `/CLAUDE.md`.

## Extraction — where the rate stands
Idle production is **~7,4 Mbit/s** on an OV5647 at RAW8, **~20,5 Mbit/s** at RAW10 1920×1080, and
**~20 Mbit/s on an IMX219** (1640×1232, `ms_extract` ~89 ms per pair idle and loaded, a pair
every ~100 ms = 3 frames, against 66 on offer). On the IMX219 that is also the **loaded** rate:
since the block reader  the consumer outruns production (`waits` > 0), so the RAW10 extractor
is the session's clock.
⚠ **The ~89 ms depend on the cache autoload** (hardware prefetch): `cam_autoload_arm()`
re-arms L1 and L2 before EVERY pair over that pair's two buffers. Armed once, the L2 switches itself
off and `ms_extract` falls back to ~120 (4 frames). `/camtest` `al_l2_ctrl` must read ENA (bit 0) set.
⚠ **The pair cycle is quantised to whole frames (~33,3 ms)**: the CSI driver overwrites
its last buffer until a free one is queued, so the second frame of every pair waits for the next
frame end. A faster extractor pays only when it crosses a frame boundary (3 → 2 frames needs
`ms_extract` below ~64 ms); in between the saving turns into `ms_wait`.
- ⛔ **On an OV5647** nothing done to the extraction path can raise the **idle** rate  — that
  sensor is the ceiling. It does not hold for the IMX219. Prove any extractor change with
  `ms_extract` under load, never at idle.
- **The live RAW10 extractor on an IMX219 is `cam_extract_raw10_lsb()`** — stream, monitor and
  stuck verdict, no zero count, no pixel sum, so its `mean_px` / `zero_diff` / `cam_px` read 0.
  `/linearity` switches it to `cam_extract_raw10_fast()` (adds both) for its run; an OV5647 on
  RAW10 always runs `_fast`. The reference is the plain loop (`cam_extract_raw10_ref()`).
  `/camtest` reports `_fast` as `r10_*` / `ns10_*` / `ms_pair_r10_*` and `_lsb` as `r10l_*` /
  `ns10_lsb*` / `ms_pair_r10_lsb*`; `equal` and `ms_pair_ext` stay the RAW8 path. ⛔ Same rule as RAW8: never change
  the live extractor without `r10_equal` and `r10l_equal` true on the target.
- ⚠ **Per-word statistics run in batches of 64** (`process_words()`), flushed at the end of every
  pair. Anything that resets the ring or the statistics must stay at a pair boundary, where
  the batch is empty.
- `/diagjson` publishes the per-pair split on every node: `ms_pair` = `ms_wait` + `ms_extract` +
  `ms_rest`.
- **`GET /camtest`** runs the byte-wise reference against the word-wise path on the node's own
  silicon; `cam_extract_ref()` stays compiled in as the definition of correct.
  ⛔ **Do not change the live extractor without it.** 409 while measuring; master only.
  ⚠ Its `ns_*` vary ~10 % (it benchmarks while the capture task extracts) — for live cost read
  `ms_extract` from `/diagjson`.

## OV5647 — on-chip processing and buffer layout
**OV5647 on-chip processing:** the driver's format table sets `0x5000 = 0xff`; `ov_boot_apply()` clears bits 7/2/1/0 (lens shading, black/white defect-pixel cancel, colour interpolation) → `0x78`. ⚠ Bit 0 acts on RAW — set, the LSB carries lag-2/lag-4 autocorrelation and a lower bias. Bits 6/5/4/3 stay on; bit 5 (RAW gamma) shows no effect on the LSB (bit naming from the OV5640/OV5645 tables in the same driver). The black level target `0x4009` is **128** DN (reset default 16 — the driver never writes `0x4009`). ⚠ At 16 the dark noise sits at code 0 and is clipped — LSB bias 0,35–0,46, σ 1,6–2,6, Bayer-periodic autocorrelation; the lens-shading gain hid exactly that. Both are firmware defaults applied at boot (`ov_boot_apply()`); per-node NVS overrides — `POST /expose?isp=0|1`, `?reg=<r>&val=<v>` (up to 8, written last), `?clr=1`, then `/reboot`; `?rd=<r>` reads a register. ⚠ **Never write these registers while the sensor streams** — it stalls the OV5647's MIPI output without tripping `stalls`. `/expose` and `/calibrate` report `isp`. ⚠ The layout
comes from the FOURCC (`BG10`), not from `sizeimage` — esp_video leaves that 0. `GET /camtest?dump=1` shows what the buffer really holds.

## Sweep — rungs and gates
**An IMX219 does not ladder: dark operation**  (`CAM_IMX_DARK` 1 in camera.c). Exposure fixed
at `IMX_DARK_EXPOSURE` 1600 lines (the longest the 30 fps frame allows, so leaking light is as
visible as possible), from boot. The sweep measures the entry setting and that rung — a health check,
never a choice — so it never moves the rung and the settle pause never fires. Its gates: autocorr,
stuck, bits, relative σ — nothing on light (no DARK/ZDIFF).
An uncertified node keeps measuring on 1600 and the Log names it ("NO certified setting").

Gates a rung must clear: autocorr < `CAL_AUTOC_TOL` 0,03 (⚠ never subsample — it gates),
σ ≤ `CAL_RAW_SIGMA_K` 1,35 × the ladder's own best (**relative, one-sided**, after the whole
ladder), no stuck frames, `mean_px` ≥ 5,0,
`zero_diff` ≤ `CAL_MAX_ZERO_DIFF` 0,125. ⛔ `CAL_MAX_MEAN_PX` 100 is publish-only.

- A dark OV5647 still ladders the exposure and keeps the `mean_px` ≥ 5,0 floor; the fixed dark
  rung is IMX219 only.
- `raw_runs_z` is published per sweep rung and gates nothing. ⚠ 0,0 in a measurement window means
  NOT ARMED, not "perfectly random" (`raw_trans` says which).
- Between rungs the sweep discards `CAL_STEP_SETTLE_PAIRS` 8 frame pairs (~0,44 s) before scoring
  the rung — driver queue plus margin. The ~1 min drift is not waited out per rung: it moves
  the bias, which selects nothing, and 9 × 60 s would cost a third of a round.
