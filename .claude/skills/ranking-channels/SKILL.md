---
name: ranking-channels
description: What each Z* column (Δn, Mau, Disp, Ser, Byt, Sync) computes per node, its thresholds and display, the worked stack, and the wire tags. Read before touching zstar_of(), the column code or the node-side statistics.
---

# The ranking channels — per column

The root `CLAUDE.md` (**The ranking channels**) holds the formula, the weights, the Z column, the
common pipeline of Mau/Disp/Ser/Byt (per-node centring, Stouffer, ÷ the block's σ of the combine)
and how Δn is read. This file holds what is specific to each column.

- **Δn** — σ across the contributing nodes of their block-centred z, each node divided by ITS OWN σ over that block. Δn = 0,82 and α = 1 → 1 + (1/0,82 − 1) = 1,220. Δn ≤ 0 or missing → factor 1. The cell prints the node count: `0,79 (3)`, highlighted below 0,6 and dimmed above 1,0. ⚠ **—** until the item's block has been centred, and whenever fewer than two nodes have a block σ — 0 would read as perfect agreement. ⚠ Small Δn is agreement, not evidence — centring has already removed the block-wide common mode that would otherwise produce it.
- **Mau** — Maurer's universal test (NIST SP 800-22 §2.9) over the item's own window, per node over exactly the bits z counts (`gcp_last_maurer()` in `elotto_gcp`, `,mau=`, `/camlog` `mau`): L = 8 (byte blocks, MSB first), Q = 2560 init blocks, K = the rest; z = (f_n − 7,1836656) / (c·√(3,238/K)), c = 0,7 − 0,1 + 8·K^(−3/8)/15. − = more compressible, coloured at |Mau| > 3. Mau = −1,5 and β = 1 → 2,5. ⚠ Below NIST's minimum at short `?run=` (L = 8 wants ≥ 2,07 Mbit per window); the c(L,K) correction keeps σ right, the centring removes the per-camera offset.
- **Disp** — per node the camera's per-mini-run σ over its window (3200-bit pieces; `,wsig=`, `/camlog` `wsig`). + = the cameras' ones-rate wandered inside the window more on this item than on the rest of the block, together; − = more evenly. Coloured at |Disp| > 3. Disp = −1,2 and ζ = 1 → 2,2. ⚠ A node publishes no window σ below 200 pieces (640 000 bit) and is then missing from the combine.
- **Ser** — per node the Stouffer z of its window's lag-1..4 bit agreement (`gcp_last_serial()`, `,ser=`; the four lags are the `acz` of its `/camlog`). + = the cameras' bits repeated more on this item than on the rest of the block, together. Coloured at |Ser| > 3. Ser = 0,8 and ε = 1 → 1,8. ⚠ The node reduces the four lags to one number; the master only centres and combines. ⚠ One camera's excursion is diluted by √k here — it stays in that node's `/camlog` (`acz`, `bchi`, `rec`).
- **Byt** — per node the window's χ² of the 256 byte values against p^w·q^(8−w), as z = (χ² − 254)/√508 (`gcp_last_bytechi()`, `,byt=`, `/camlog` `bchi`). + = the cameras' byte values were more uneven on this item than on the rest of the block, together. Coloured at |Byt| > 3. Byt = 0,5 and η = 1 → 1,5. ⚠ A node sends none below 5 bytes per value (10 240 bit).
- **Sync** — the cameras' synchrony INSIDE the window, shown in the item hover text, Save's `Sync` column and `sync` in `/extremes`. Every node logs its frame pairs with their time since the flush request that follows `M` (the array's common zero, within ~1 ms) and cuts the window into `EL_SLOTS` 10 time slots of `el_slot_us(nseg)` (`elotto_link.h`; nominal window + 0,2 s over 10, at least 250 ms); a pair is split between the two slots whose centres it lies between. A slot z is against the window's own bias, so neither Z nor a node offset is in it (`camera_win_slots()`, `,sl=`). The master sums, per slot, the products of every two combined nodes' slot z, over all slots, divides by √(number of products), then centres on the block mean and divides by the block's own σ — read against 0, + = the cameras moved together. ⚠ Pass items only — a scoring row has none. ⚠ Products only, never a node's own square: one camera's dispersion is Disp. ⚠ The cameras free-run, so a pair near a slot edge sits in different slots on different nodes; the split damps that, it does not remove it.

Worked stack, all weights 1: Z/σ_z = −2, Disp = −1,2, Δn = 0,82, Ser = 0,8, Mau = −1,5, Byt = 0,5 → −2 · 2,2 · 1,220 · 1,8 · 2,5 · 1,5 ≈ −36,23.

The formula at the top of the page leaves out a factor whose weight is 0 (at κ = 0 the base reads `sign(Z)`); the Save params file keeps all of them. A reload of a finished session shows that session's weights until an input changes.

## Wire
`Z:<z>[,wsig=<σ>][,mau=<z>][,ser=<z>][,byt=<z>][,sl=<v>/…]`, all TAGGED (`,sl=`: ten slot z in hundredths, empty = none).
`,wsig=` (window σ) feeds the `Disp` column and the jump board, `,mau=` (window Maurer z) the `Mau` column, `,ser=` the `Ser` column, `,byt=` the `Byt` column; the master takes its own from `gcp_last_maurer()` / `gcp_last_serial()` / `gcp_last_bytechi()`. Every node measures the commanded `nseg`.
