# Where things stand

Project **HISTORY snapshot**, not a rule. Rules: [`../CLAUDE.md`](../CLAUDE.md). Evidence: [`DECISIONS.md`](DECISIONS.md).

## Current (2026-09-01)

**Instrument:** D65 LSB stream + concordance ranking (`?wpre=` = Conc-Gewicht). D66: always unattended; HTML **Now:** card. D67: rounds until Abort is the only session. D68: Z* in **block-σ** units. D69: scoring centres per node like the pass. Sweep `|bias−0,5|`; σ-Gate relativ (`CAL_RAW_SIGMA_K` 1,35). Soft-down 1,35 × Peer-Median-σ. Node table has **no p-column** (always 0 at large n; gates nothing). Soft-down origins title includes wall time (`t_ms`).

**On the rig.** Master `fw_sha` `3a947dd7360c08de` (`6aea6c8`, D68/D69, ota_1) — UI-drop-p / trip-time **not flashed yet**. Slaves `2465c2251beb01b6` (D65).

**Overnight session** (aborted, ~17,5 h, `run=0,5`, `wpre=0,1`, 328 Blöcke, 20 674 Items): `pass_σ` 1,027, void/stalls/lost 0, Pairwise max |r| 0,015. Soft-down **6× slave2** (`.155`; eine heftige Episode Bl. 193–195, σ bis 7,4, sonst Stunden bei Median 1,01) und **1× Master** (Bl. 231). Jump-Board: slave2 allein, nicht alle vier — gemeinsame 5-V-LED-Schiene unwahrscheinlich. `excl` 252.

Idle production ~7,4 Mbit/s (D65, 2× words). `RUN_SEGS_REF` 141026 still predicted.

×√n array gain still **not established**.
