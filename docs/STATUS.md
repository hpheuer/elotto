# Where things stand

Project **HISTORY snapshot**, not a rule. Rules: [`../CLAUDE.md`](../CLAUDE.md). Evidence: [`DECISIONS.md`](DECISIONS.md).

## Current (2026-09-07)

**Instrument:** D65 LSB stream + concordance (`?wpre=`). D66 always unattended (HTML **Now:** card). D67 rounds until Abort. D68 Z* in **block-σ** units. D69 scoring centres per node like the pass. D75 unbounded key, per-item channel weights. D76 one block = one round (`?maxruns=`). D77 sign test on **centred** halves. D78/D78b one sortable Top-10 over `GET /extremes` (poll 5 s, display pool 50). Sweep `|bias−0,5|`; σ-Gate relativ (`CAL_RAW_SIGMA_K` 1,35). Soft-down 1,35 × Peer-Median-σ. Illumination board: `tools/tune.html`.

**On the rig.** Last noted flash: Master `fw_sha` `ba6cf3ee71c84bac` (`2979ead`, ota_0). Slaves `2465c2251beb01b6` (D65). Commits after that (D77, D78, D78b, tune.html) are on `master` and may not be on the boards.

**Overnight session** (aborted, ~17,5 h, `run=0,5`, `wpre=0,1`, 328 Blöcke, 20 674 Items): `pass_σ` 1,027, void/stalls/lost 0, Pairwise max |r| 0,015. Soft-down **6× slave2** (`.155`; eine heftige Episode Bl. 193–195, σ bis 7,4, sonst Stunden bei Median 1,01) und **1× Master** (Bl. 231). Jump-Board: slave2 allein, nicht alle vier — gemeinsame 5-V-LED-Schiene unwahrscheinlich. `excl` 252.

Idle production ~7,4 Mbit/s (D65, 2× words). `RUN_SEGS_REF` 141026 still predicted.

×√n array gain still **not established**.
