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
- **The live RAW10 extractor is `cam_extract_raw10_fast()`; the reference is the plain loop**
  (`cam_extract_raw10_ref()`). `/camtest` reports the RAW10 pair as `r10_*` / `ns10_*` /
  `ms_pair_r10_*`; `equal` and `ms_pair_ext` stay the RAW8 path. ⛔ Same rule as RAW8: never change
  the live extractor without `r10_equal` true on the target.
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
