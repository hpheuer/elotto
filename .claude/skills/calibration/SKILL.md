---
name: calibration
description: The camera sweep in detail - rung choice and keep margin, dynamic sweep interval, /calibrate, the cam_* fields in /loops and what they do not mean. Read before touching calibrate_all()/camera_calibrate() or interpreting /calibrate or /loops cam_*.
---

# Camera calibration — details

The standing rules (⛔ bias selects nothing, ⛔ gain 1023 / exposure only, ⚠ 60 s settle after a
rung change, ⚠ never judge `raw_sigma` from single `/expose` readings) stay in the root
`CLAUDE.md` under **Camera calibration**. IMX219 specifics, gate thresholds, `raw_runs_z` and the
per-rung settle: `components/elotto_camera/CLAUDE.md`.

- At every insertion the master broadcasts `K<budget_ms>,<segs>`, sweeps its own ladder in
  parallel, and waits for every node's `OK:`. Each node keeps the rung with the **lowest
  `raw_sigma` among candidates that clear the gates**, falling back to the bare gate if none qualify.
- ⚠ **The incumbent rung is KEPT** unless a challenger beats it by `CAL_KEEP_MARGIN_K` 3 SE of its
  own measurement (`kept` in `/calibrate`); for σ that SE is σ/√(2(m−1)) over m mini-runs.
  ⚠ **The σ key is coarse**: at the 10 s default budget 3 SE is ~5 %, so a smaller gain will not
  move a rung. SE goes as 1/√budget, so `?cal=40000` halves the bar to ~2,4 %. The incumbent is
  what runs when the sweep STARTS, so a manual `/expose` gets one sweep of protection — deliberate.
- The budget is a **cap, not a target** (default 10 s, `?cal=<ms>`). **When** is the dynamic
  interval (`calibrate_all()`): the session-start sweep always runs; after it a sweep runs at the
  next candidate point — the end of every scoring pass, before the pass, the round boundary — once
  the interval has run out: `CAL_DYN_MIN_MS` 15 min, doubled by every ok sweep (all nodes
  certified, no exposure moved) up to `CAL_DYN_MAX_MS` 2 h, back to 15 min after any other sweep or
  a soft-down trip (`calibrate_shorten()`). `/status` `cal_interval_ms` / `cal_due_ms`;
  `cal_did_sweep` / the `/loops` `cal_ms` mean "a sweep ran in this round". The page's **Log** card
  shows every sweep, change, pause and soft-down with wall time (`GET /loops?ev=1`).
- The 60 s settle pause (`CAL_SETTLE_AFTER_MS`): after a rung change this rig's cameras need ~1 min
  to settle (px still climbing, bit bias moving by ~0,002), and the items measured in that minute
  sit far enough from the block mean to trip soft-down on their own. A sweep where every node kept
  its rung pauses nothing. It fixes only the trips that follow a change.
- `GET /calibrate` serves the whole last sweep per rung with the gate each failed plus the raw
  stats, **on every node** — a per-node optical fault is diagnosable. The chosen setting is
  recorded per block in `/loops`.
- **`/loops` also carries `cam_sig` / `cam_rsig` (stream σ) and `cam_px`**.
  ⚠ **They are cumulative since the last SWEEP, not per block** — `camera_stats_reset()`
  runs only in the sweep and on `/expose`. The sweep before the pass resets them, so a
  block's row spans **that block's own pass** — a short span and therefore a noisy σ. ⚠ A `?cal=0`
  session has no reset at all. For anything that has to be located in time use **`GET /camlog` on
  the node** — never these. `/camlog` is the record that does not depend on the master keeping the row.
  ⚠ `cam_px` 0 = the node did not report it, **not** a dark frame.
  ⚠ Every other `cam_*` field is what the last SWEEP found: two blocks on one setting carry an
  identical `cam_bias` and describe neither.
  ⚠ `cam_rsig` is recorded and never gated — no null of 1, non-stationary per node.
- ⚠ `camera_get_stats()` is cumulative since the last sweep; in a `?cal=0` session they are
  lifetime averages.
- `camera_calibrate()` passes the gain in force at entry (`g0`) to every rung, so
  `CONFIG_ELOTTO_CAM_REG_GAIN` is the operating point for the life of the board, not a boot value —
  and a `POST /expose?gain=` override is carried forward by the next sweep instead of being replaced.
