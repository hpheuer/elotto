# Where things stand

Project **HISTORY snapshot**, not a rule. Rules: [`../CLAUDE.md`](../CLAUDE.md). Evidence: [`DECISIONS.md`](DECISIONS.md).

## Current (2026-08-31)

**Instrument (code, D65):** four-node photon LSB-diff array; adjacent-pixel XOR **off**; one LSB z plus
concordance ranking (`?wpre=` = conc weight). Sweep selects on `|bias−0,5|` with incumbent hysteresis
`[D46]`; σ gate is relative (`CAL_RAW_SIGMA_K`). Soft-down: 1,35 × peer-median σ. Wire
`Z:<z>,<h1>,<h2>[,wsig=]`.

**Not yet on the rig.** Master+slave built; a live Unlimited Euro session is still on the previous
image (`fw_sha` `d61419b5b5759ce8`). Flash all four together after that session ends. Then
re-measure `focus_win_ms` vs `?run=` — `RUN_SEGS_REF` 141026 is predicted.

**Idle/load rates** from the prior instrument (5,71 / ~3,7 Mbit/s) do not apply after D65.

×√n array gain still **not established** — judge on per-block combined σ **and** the full pairwise
matrix.

### Pooling
Never mix across the splits in CLAUDE’s pooling table / `[D1]`. D65 sessions do not pool with
anything from the prior instrument.
