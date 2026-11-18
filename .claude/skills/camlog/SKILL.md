---
name: camlog
description: Read a node's GET /camlog per-window log - entry fields (acz, bchi, wsig, mau, ...), the rec session record, what it holds beyond the wire.
---

# Reading `GET /camlog`

The root CLAUDE.md keeps the standing rules: the log is a ring (pull it inside the session, per
node) and `t_ms` is each node's own uptime (align by `tag` or by ordinal).

- Entry: `t_ms` `tag` `wsig` `wn` `wlo` `mau` `acz` `bchi` `rsig` `rbias` `sig` `bias` `px` `ac1` `zdiff`.
  `mau` = Maurer z over THIS window (null = none); ⚠ `ac1` is cumulative since the sweep.
  **`acz`** [4] = THIS window's lag-1..4 bit agreement as z, in read order and across word
  boundaries, against the agreement rate p² + q² of the window's own ones-fraction p (so the LSB
  bias does not read as correlation); + = bits repeat; lag 1 is the runs statistic. **`bchi`** =
  THIS window's χ² of the 256 byte values against p^w·q^(8−w), as z = (χ² − 254)/√508. Both unit
  normal for random bits, computed in `gcp.c` beside Maurer, The four lags feed Ser, `bchi` feeds Byt.
- **`rec`** in the reply header is their session record, which the ring cannot lose: windows
  counted (`n`), how many had |z| > 3 (`hit`; chance gives 0,27 % of `n`) and the largest |z| with
  its tag (`max`, `tag`), in the order lag 1, 2, 3, 4, bchi. Cleared at every session start (on a slave: when it answers discovery), not by a pause.
  `wlo` = 1 when `wsig` sits more than 3 SE (1/√(2(wn−1))) below 1.
  `tag` = combination id on the master, answered `M` sequence on a slave, **0 = a scoring run**.
- It records what never travels on the wire — `acz`, `rsig`, `px`, `ac1`, `zdiff`.
  The wire carries `wsig`, `mau`, `ser` (the Stouffer z of the four lags), `byt` (`bchi`) and the slots, nothing else. That difference is what separates "light leaked in" from "this sensor
  is dispersing".
- ⚠ Only windows that produced a **Z** are pushed; a faulted or voided run has no window.
