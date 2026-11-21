# main/ — master firmware

## elotto.c — webserver and page
- ⚠ The URI-handler cap fails silently (404, return value unchecked) — the count lives at
  `start_webserver()`; prefer `?all=1` on an existing endpoint over a new handler.
- ⚠ **`max_open_sockets` 13 (lwIP 16 minus the 3 httpd reserves) exists so the UI cannot lock an
  external client out**. With a smaller table one open page fills it, and `lru_purge_enable`
  then evicts each newly accepted, still-silent client before its request is read — `curl` sees a
  successful TCP connect followed by an immediate close, every time, while the page keeps updating.
  ⚠ That failure looks exactly like a dead HTTP task. **Open a second browser window**: if it
  loads, the board is fine and the caller is being evicted.
- **`GET /diagjson?all=1` is the COLLECTOR**: the whole array's front-end health in one request,
  discovery order, IP per row. 409 while measuring. ⚠ `cal_*` is what the last SWEEP chose;
  `exposure`/`gain` are LIVE — they differ after a manual `/expose` or an uncertified sweep.
- `POST /expose?exp=<lines>[&gain=<g>]` sets one node's live operating point (trying a setting
  against a live reading; resets camera stats so `mean_px` answers in ~2 s). Not sticky —
  the next sweep overwrites it, correctly. 409 while measuring.
- `tools/tune.html` (repo root) is the live per-node exposure/health board (`/linearity` + last
  sweep), opened on the operator PC; 409 while a session runs. Not served by the master.

## Resources
- **PSRAM is mandatory**: capture buffers, the LSB-ones side ring (`s_ring_raw`), `loop_hist`,
  per-item per-node archives (`s_node_z` and the Mau / Ser / Disp / Byt copies, sized to `NUM_RUNS`),
  the scoring-sum log (`SCORE_SNAP_MAX` 128 snaps, ~34 KB) and the camera-jump board
  (`WSIG_KEEP_N` 100 rows, ~7 KB), and the full table (`FULL_MAX` 250000 rows, ~13 MB of the
  ~22 MB free in a session).
  The word ring `s_ring` (64 KB) is **internal RAM**; `results[]` stays internal too:
  `NUM_RUNS` 1400.
- ⚠ **The measurement path reads the ring in BLOCKS** — `camera_read_words()`, 112 words per call
  from `gcp_zscore_raw()`. `camera_read_word()` takes `s_mutex` and fences four times per
  32 bits; read that way, the reader — not PSRAM, not the camera — sets the session rate. Single words are for seeding only.
- ⚠ **Task priority is load-bearing**: any task calling `camera_read_word[s]()` must run **above** the
  extraction task, or the consumer starves ~10× (see `camera.h`; signature: ring `drops` huge with
  `waits == 0`).
- ⚠ **Task CORE is load-bearing too, and it is a separate rule.** Every task in the measuring path
  is created with `xTaskCreatePinnedToCore()`: extraction on `ELOTTO_CAM_TASK_CORE`, consumer and
  web server on `ELOTTO_CAM_CONSUMER_CORE` (both in `camera.h`). Plain `xTaskCreate()` is
  `tskNO_AFFINITY`, and the master creates `elotto_task` fresh on **every** `/start` — unpinned it
  shares a core with `cam_task` in about one session in three and **everything halves**.
  ⚠ The signature is consumption halving EXACTLY while all other nodes stay normal.
  ⚠ **One fast session proves nothing here** — it was intermittent. Re-test any new task in this
  path over ~20 session starts.
- ⚠ **`elotto_task`'s stack is load-bearing too**: `ELOTTO_TASK_STACK` 16384 (`sensor.h`). The
  deepest chain is the sweep candidate after a scoring pass — `score_run()` (~4,8 KB of locals) →
  `calibrate_all()` → `camera_calibrate()` → `cal_step()` → a float `ESP_LOGI` — and needs
  ~8,2 KB; it only runs with `?passes=` > 1 once the first post-start sweep is due. An overflow
  trips the FreeRTOS canary: the master panics and reboots (`/status` `reset_reason` `PANIC`).
  `/status` `task_stack_free` is the least free stack this session — read it after adding locals
  or calls anywhere on that chain. `heap_int_*` / `heap_ps_*` (free, minimum, largest block) are
  the leak check: free must not fall from round to round.
