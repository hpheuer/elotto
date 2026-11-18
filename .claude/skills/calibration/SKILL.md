---
name: calibration
description: Camera sweep internals - the keep margin and its SE, /calibrate, the /loops cam_* fields in detail, how the gain is carried through a sweep. Read before touching calibrate_all()/camera_calibrate() or interpreting /calibrate or /loops cam_*.
---

# Camera calibration — internals

The rules, the rung choice and when sweeps run are in the root `CLAUDE.md` (**Camera
calibration**); IMX219 specifics, gate thresholds, `raw_runs_z` and the per-rung settle in
`components/elotto_camera/CLAUDE.md`.

- **Keep margin**: a challenger must beat the incumbent by `CAL_KEEP_MARGIN_K` 3 SE of its own
  measurement (`kept` in `/calibrate`); for σ that SE is σ/√(2(m−1)) over m mini-runs.
  ⚠ **The σ key is coarse**: at the 10 s default budget 3 SE is ~5 %, so a smaller gain will not
  move a rung. SE goes as 1/√budget, so `?cal=40000` halves the bar to ~2,4 %. The incumbent is
  what runs when the sweep STARTS, so a manual `/expose` gets one sweep of protection — deliberate.
- `cal_did_sweep` / the `/loops` `cal_ms` mean "a sweep ran in this round". The page's **Log** card
  shows every sweep, change, pause and soft-down with wall time (`GET /loops?ev=1`).
- `GET /calibrate` serves the whole last sweep per rung with the gate each failed plus the raw
  stats, **on every node** — a per-node optical fault is diagnosable. The chosen setting is
  recorded per block in `/loops`.
- **`/loops` `cam_sig` / `cam_rsig` (stream σ) and `cam_px`**: `camera_stats_reset()` runs only in
  the sweep and on `/expose`. The sweep before the pass resets them, so a block's row spans **that
  block's own pass** — a short span and therefore a noisy σ. ⚠ A `?cal=0` session has no reset at
  all; `camera_get_stats()` then returns lifetime averages. `/camlog` is the record that does not
  depend on the master keeping the row.
  ⚠ `cam_px` 0 = the node did not report it, **not** a dark frame.
  ⚠ Every other `cam_*` field is what the last SWEEP found: two blocks on one setting carry an
  identical `cam_bias` and describe neither.
  ⚠ `cam_rsig` is recorded and never gated — no null of 1, non-stationary per node.
- **Gain through a sweep**: `camera_calibrate()` passes the gain in force at entry (`g0`) to every
  rung, so the boot gain (clamped by `cam_gain_max()`) is the operating point for the life of the
  board — and a `POST /expose?gain=` override is carried forward by the next sweep instead of being
  replaced.
