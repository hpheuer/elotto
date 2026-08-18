# `-Og` baseline profile — 2026-08-18, before the extraction work

The **"before" half of the 1,67× claim**, and the only copy of it. These are raw
`/diag` snapshots taken from all four nodes on the firmware that preceded
commit `cfd6e1c`, i.e. `-Og`, `__builtin_popcount` calling `__popcountsi2`, and
the byte-at-a-time extractor. Reproducing them means deliberately rolling all
four nodes back to that binary, so they are kept rather than regenerated.

| files | node |
|---|---|
| `master*.json` | master |
| `n103_*.json` | slave0 |
| `n145_*.json` | slave1 |
| `n155_*.json` | slave2 |
| `status*.json` | the master's `/status` alongside |

`*1` / `*2` are two snapshots ~8 s apart, so a rate can be differenced out of the
cumulative counters rather than trusted from `mbit_s`; `*_load*` is the same pair
taken while a session was measuring. `analyze.py` does that differencing.

**Idle, `-Og`: ~3,36 Mbit/s per node.** Against 5,71 today.

⚠ **Not comparable field by field with anything taken after 2026-08-18.**
- `mean_pixel` changed meaning: it was a stride-16 sample from
  `accumulate_pixel_level()`, it is now every pixel, folded into the diff loop.
- The exposure rungs differ (these were taken at exp 64 on the master).
- The per-pair timing split (`ms_pair` / `ms_wait` / `ms_extract` / `ms_rest`)
  did not exist yet, which is precisely why the 14 ms went unexplained for a
  day — the rate was visible and its composition was not.
