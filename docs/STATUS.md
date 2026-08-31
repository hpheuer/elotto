# Where things stand

Project **HISTORY snapshot**, not a rule. Rules: [`../CLAUDE.md`](../CLAUDE.md). Evidence: [`DECISIONS.md`](DECISIONS.md).

## Current (2026-08-31)

**Instrument:** D65 LSB stream + concordance ranking (`?wpre=` = Conc-Gewicht). D66: always unattended; HTML **Now:** card via `GET /focus`. Sweep `|bias−0,5|`; σ-Gate relativ (`CAL_RAW_SIGMA_K` 1,35). Soft-down 1,35 × Peer-Median-σ. Wire `Z:<z>,<h1>,<h2>[,wsig=]`.

**On the rig.** Master `fw_sha` `203708f4f85eaeea` (`f2415ae`, D66, ota_1). Slaves `2465c2251beb01b6` (D65, no UI change). `?focus=` 400, `/ready` 404.

**Sweep + linearity** (aborted session, all four): light steady (ratios ≈1,8…2,0). Dark rungs fail DARK/ZDIFF. RSIG rejected nothing extra. Autocorr takes the bright end. slave1 still brightest (`px/exp` 0,25 vs slave0 0,13), chose exp 256 at ac 0,026 — under the bar, not a code fault.

Idle production ~7,4 Mbit/s (D65, 2× words). `RUN_SEGS_REF` 141026 still predicted.

×√n array gain still **not established**.
