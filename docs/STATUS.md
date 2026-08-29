# Where things stand

Project **HISTORY snapshot**, not a rule. Rules: [`../CLAUDE.md`](../CLAUDE.md). Evidence: [`DECISIONS.md`](DECISIONS.md).
Older dated narratives (2026-08-18…27 flash/session logs) were trimmed 2026-08-28; recover with
`git show 1e62bca:docs/STATUS.md`.

## Current (2026-08-29)

**Instrument:** four-node photon LSB-diff array; v3 single-pass + Unlimited; block centring;
ranking key = weighted `(z_ctr, z_conc)` — half-window leave-one-out `[D56]`.
Baseline phase **deleted** `[D48]`. Spectral-entropy **deleted** `[D53]`. Runs ranking **deleted** `[D55]`.
Nearest-zero table **deleted**; unlimited compact keeps 100 extremes `[D56]`.
`null_flags` / NB / CUSUM **deleted** `[D47]`.
Sweep selects on **pre-fold** bias with incumbent hysteresis `[D46]`. `/loops` records in-block
`cam_sig` / `cam_rsig` / `cam_px` `[D50]`. Measuring window **0,5–5 s** `[D51]`.

**Idle rate** ~5,71 Mbit/s per node; loaded ~3,7 — ceiling is the sensor at idle `[D23]``[D25]`.

**UI (master):** GCP primary line (`pass_σ`, `v_eff`, `|r|√n`, soft-downs) on the results card.
Form remembers last start values including focus (NVS, `confirm=1` only) `[D49]`.
Bonferroni line **deleted** `[D57]`.

### Open / not fully exercised
- Relative pre-fold dispersion gate: never observed rejecting a rung the folded-σ gate did not
  already reject `[D46]`.
- Pre-fold channel over many real 15-minute blocks (most checks used short `calint`).
- ×√n array gain still **not established** — judge on per-block combined σ **and** the full
  pairwise matrix (CLAUDE).

### Pooling
Never mix across the splits in CLAUDE’s pooling table / `[D1]`. Different `?run=` ⇒ different bit
count per item.

**Dropped and deferred:** last section of [`DECISIONS.md`](DECISIONS.md) — check before proposing
anything that sounds obvious.
