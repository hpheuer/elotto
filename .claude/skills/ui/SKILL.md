---
name: ui
description: The master's web page and its readouts - parameter line, stat cards, Top-10 /extremes, Save files, camera-sigma jump board, soft-down origins, the Runs per round preview (rate model in sensor.h), start-form memory. Read before editing the page in main/elotto.c, changing the rate model, or interpreting its tables.
---

# UI — what bites

What the page shows is in `main/elotto.c`; this keeps what bites. The cross-file rules
(⛔ jump board is a suspicion list, ⚠ /extremes poll and click limits, start-form memory changes no
API default) are in the root `CLAUDE.md` under **UI**.

- The **parameter line** is built from `/status`, not the form — a curl-started run and a reloaded
  page both label themselves. ⚠ Its measured "per run" reads high early — `elapsed_ms` also
  contains the opening sweep.
- **Stat cards: the split is load-bearing** — top row round-relative in every figure, bottom row
  session-relative; the scoring has its own round-relative row.
- **Top-10** — `GET /extremes?sort=key|z|disp|nsd|ser|mau|byt&dir=high|low`, `?n=1..1000` (default 10):
  the held rows best on that column and direction, chosen on the master. ⚠ **Held rows** are the
  compaction archive (chosen on the pool criterion) plus the open round, so while the session runs
  another column's extremes are those of that archive, not of the session; after the session the
  set is every ranked pass item (the full table). Without `?sort=` the set is chosen on the pool
  criterion. `dir=` without `sort=`, an unknown value and a truncated value or query (> 95
  characters) answer **400**. A row without a value in the sorted column is not listed. The sort
  never changes the compaction archive or the pool.
- **Save** writes four files in one step (CSV for a German Excel: `;`, decimal comma, BOM):
  the pass items, `elotto_sums_…` (`/extremes?sums=1`, every number's seven signed sums for every
  scoring still in RAM), `elotto_params_….txt` (the parameters that ran, both windows, the formula
  with its weights) and `elotto_jumps_….csv` (`/extremes?jumps=1`, the whole jump board). The pass
  items once the session is over: `elotto_all<n>_…`, every ranked pass item plus the quarantined ones, from
  `/extremes?full=1&from=<i>` in pages of 2000 index slots (`span`, `next`), written last; if it
  cannot be had the held extremes are written instead. While the session runs `full=1` answers
  **409** and the file is `elotto_top…` from `/extremes?n=1000` (`crit`/`dir` in the reply and the
  file name). A failed sums fetch skips that file; a failed jumps fetch falls back to the ten
  `/status` rows.
  ⚠ The folder dialog exists only on a secure page; on the master's http address the files go to
  the browser's download folder.
  ⚠ In **scoring-only mode** (`maxruns=0`) the first file is built client-side from the sums — the
  numbers ranked by the selected criterion (`elotto_scored_<crit>_<dir>_…`).
  ⚠ A continuous scoring's true pass count is the snap's `round`, not `passes` (the per-number pass
  counter saturates at 255).
- **Camera-sigma jump board**: the largest changes in a node's own camera noise from its previous
  measurement to this one, session-wide (`WSIG_KEEP_N` 100 rows, PSRAM; `/status` `wsig` carries
  the first ten). Only a node that was in that item's combine enters it; every node's jump still
  feeds `wsig_sd`. A row's measurement values are provisional until its block is centred; a scoring
  run's are empty until that pass closes.
  ⚠ One row per NODE: two nodes on the same item is a shared cause, one node alone is that camera.
  ⚠ Read the **×σ** column, not the raw jump: it scales against `wsig_sd`, the session's own
  measured jump noise. There is deliberately **no minimum jump** — a quiet session shows ten rows
  at 2..3 σ.
  ⚠ Scoring windows enter it too, named `scoring <pass>/<round>` — not an item identity.
- **Soft-down origins**: when a block trips a node, the three measurements of that block whose z
  sat furthest from the block mean, captured at block close because compaction takes the rows one
  round later. Time is `now − (uptime_ms − t_ms)` — no RTC.
  ⚠ A trip belongs to a BLOCK, not to a measurement — σ is the spread over that round's
  `?maxruns=` items. This names what carried the spread, not what "caused" it.
  ⚠ The ×σ column is measured against that block's own spread, not the null: near 3 is
  ordinary. Read the SHAPE — one item far out is an excursion, three close together mean the
  block was simply wide.
  ⚠ The block number shown is 1-based like `/loops`; `results[].block` is 0-based.
- The **Runs per round** field previews pool size and round length from the rate model in
  `sensor.h`: `cycle_ms ≈ 224·segments/rate + CYCLE_FIXED_MS + gap_ms`; the page takes
  `CYCLE_FIXED_MS` and the rate pair from the headers at build time; the live ETA uses
  measured pace. ⚠ It prints the length **split into scoring and pass** and warns about nothing
   — the field moves only the pass, which at 20 scoring passes is under 1 % of a Eurojackpot
  round.
- **The start form remembers its last values** (NVS, survives reboot and OTA); only the web UI
  sends `confirm=1`. A curl start that omits `kdn` / `kmau` / `kser` / `kdisp` / `kbyt` runs at 0
  and does not overwrite the stored form weights. Mode is not remembered — it is which button was
  pressed. The pool criterion is remembered by its own rule
  (written on the Σ click).

## Current-item display
A "Now:" card shows the number being scored or the combination being measured, in large type.
The session is always unattended.
- The card updates ~70 ms **before** the bits start ⛔.
- `GET /focus` (~60 B) polled at 10 Hz, separate from the 2,5 KB `/status`; `seq` is monotonic per
  window, so the UI counts *missed* windows — a skipped window names the wrong numbers.
- `POST /pause?on=1|0` holds **between** runs only; state stays `running`, paused time is excluded
  from `elapsed_ms`.
- ⚠ `?focus=` answers **400**.
