# PLAN: Networked 4-Node Array — UDP sync, Ethernet OTA, no USB

Status: **new (2026-07-25)**. Supersedes **Phase 4 of `PLAN_4NODE.md`**; that document remains
the contract for everything about the *noise source and the statistics* (Phases 0–3 — camera
entropy, studentization, permuted order, Stouffer accumulation, coverage, independence and
drift checks). This document owns **transport, provisioning and firmware delivery** only.
Nothing here changes what is measured or how a z-score is computed.

## Why Phase 4's topology decision is reversed

`PLAN_4NODE.md` lists "Topology: UART point-to-point, unchanged" under *architecture decisions
(made, do not re-litigate)*. That decision was explicitly conditioned on the hardware present at
the time — two boards and one crossover cable already wired — and the same section defers the
star topology for a 3rd/4th slave to "Phase 4", with "no GPIO-matrix routing work happens now".

**The hardware assumption has changed**: 4× ESP32-P4-ETH, each with its own OV5647, plus a
4-port PoE switch. Four Ethernet nodes on a switch is a different problem than one crossover
cable, so the decision is reopened deliberately rather than drifting:

| | UART star (old Phase 4) | UDP (this plan) |
|---|---|---|
| Wiring | 3 crossovers + common GND star, GPIO-matrix routing for UART2/3 | 4 cables into a switch |
| Trigger simultaneity | 3 sequential UART writes → skew between nodes | **one broadcast datagram** — all nodes start within µs |
| Scaling past 4 | out of UARTs | unchanged |
| Firmware delivery | USB per board, or a UART block protocol | the same Ethernet link |
| Delivery guarantee | effectively lossless | needs explicit sequence + timeout |

Simultaneity is the one that matters physically: the premise is that all nodes integrate the
*same* window. A single broadcast beats N sequential writes. Latency and jitter (sub-ms either
way) are irrelevant against a 470 ms run.

## Goal

Four nodes, each on its own camera, triggered over UDP on the existing switch; firmware
delivered over Ethernet. **USB is used exactly once per board** — to install the recovery
updater — and then never again.

## Measured facts this plan is built on (2026-07-25)

Established before writing, not assumed:

- **Flash: 32 MB on both boards measured** (`esptool flash-id`), while `sdkconfig` declares
  `FLASHSIZE_2MB` with a single-app table. 30 MB per board is currently unused, so partition
  space is a non-issue.
- **Current app static RAM: 421 KB**, of which **411 KB is `libmain`** — `results[]` (320 KB),
  `s_zsum[]` (64 KB), `s_perm[]` (16 KB). The statistics tables *are* the memory profile.
- **The network stack costs almost nothing statically**: lwIP 2.5 KB, `esp_http_server` 0,
  `esp_netif` 0.03 KB, `esp_eth` 0.12 KB. Their real cost is runtime heap — pbuf pool, Ethernet
  DMA descriptors, task stacks.
- Therefore an updater image *without* the statistics tables sits near **50–60 KB static plus
  heap**, and the **130 KB working budget is realistic**. What tunes it is sdkconfig (lwIP
  buffer counts, DMA descriptor count, HTTP stack size), not code size.
- IDF v6.0.1 provides `FLASHSIZE_32MB`, `esp_ota_ops.h` and bootloader rollback.

## Architecture decisions (made)

### 1. Two images per board: an immutable recovery updater + two app slots

```
bootloader   0x2000
partition table 0x8000
nvs          data
otadata      data          <- which app slot boots
phy_init     data
factory      app   1 MB    <- the OTA-Firmware (recovery updater). Never overwritten.
ota_0        app   3 MB    <- application
ota_1        app   3 MB    <- application
(remaining ~24 MB free for later use: session logs, /loops history, coverage archives)
```

**Why a separate factory image rather than only putting OTA into the app:** "drop USB
completely" only holds if the updater is reachable *when the application is broken*. An updater
that lives solely in the app is one bad flash away from needing a cable — and with a shared
`elotto_camera` component, a bad image tends to land on several boards at once. The factory
image is never a target of OTA, so it is always there.

### 2. What the OTA-Firmware must do — everything USB does today

It is the network replacement for the serial bootloader, so it needs the same capability set:

| USB / esptool today | OTA-Firmware equivalent |
|---|---|
| `write-flash` app | receive image, write to `ota_0`/`ota_1`, verify |
| select what boots | `esp_ota_set_boot_partition` |
| `erase-flash` / erase NVS | erase data partitions |
| `flash-id`, chip info | report chip, MAC, flash size, partition table |
| version check | report running image sha256 + build id |
| reset | reboot into a chosen slot |

It contains **no camera, no GCP, no statistics** — that is the whole reason it fits in ~130 KB.

### 3. Normal updates go app → app; the updater is the recovery path

- **Fast path:** `POST /update` on the *running application* writes the inactive slot, reboots,
  self-validates, marks valid. No detour through the updater.
- **Recovery path:** a bad image fails self-validation → bootloader rollback → previous app. If
  both slots are bad, a boot-failure counter falls through to `factory`.
- The application must therefore **only mark itself valid after Ethernet is up and the
  webserver answers** — validating at `app_main` entry would defeat rollback entirely.

### 3a. Failure modes and recovery — what actually saves the board

Worth being precise, because the intuition "if it crashes I just press reset" is **only
conditionally true**. Reset does nothing but restart: the bootloader then boots whatever
`otadata` points at. If that is a broken app, reset boots the broken app again, forever. What
recovers the board is *rollback*, and `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` **defaults to
`n`** — it must be switched on deliberately in Phase A or none of this holds.

With rollback on, a freshly OTA'd app boots in state `ESP_OTA_IMG_PENDING_VERIFY`, which (per
the IDF Kconfig help) *"prevents the re-run of this app"*: unless the app confirms itself, the
next boot rolls back to the previous working app. Crash, panic reboot, watchdog, power cut or a
manual reset all trigger it equally.

Layered, weakest failure first:

| # | Failure | What recovers it | Needs |
|---|---|---|---|
| 1 | New app crashes/hangs **on first boot** | Rollback: it never confirmed → next boot reverts to the previous app | `BOOTLOADER_APP_ROLLBACK_ENABLE=y`. This is the case the reset button covers |
| 2 | New app "works" but cannot be updated | Never validate on liveness — see the criterion below | discipline in the app |
| 3 | App validated, then crash-loops later (breaks only when a session starts, Ethernet dies overnight) | Boot-loop counter in RTC/NVS: after N unhealthy boots, `esp_ota_set_boot_partition(factory)` + restart | app code, ~30 lines |
| 4 | App validated, then **hangs** without rebooting | GPIO factory reset: bootloader erases `otadata` → boots `factory` updater | `BOOTLOADER_FACTORY_RESET=y` + `BOOTLOADER_OTA_DATA_ERASE=y` + a pin |
| 5 | Corrupt bootloader or wrong partition table | **USB. Irreducible** — OTA cannot rewrite what loads OTA | one cable, kept |

**Case 3 is the one that breaks the naive model.** Once an image is marked valid, rollback is
disarmed permanently; reset just reboots into it. That is why the factory updater exists, and
why cases 3 and 4 need their own mechanisms rather than trusting the button.

**The validation criterion is therefore "can I still be updated?", not "is the app correct?"**
Mark valid only once Ethernet is up *and* the HTTP server is answering. An app that validates
is by construction remotely updatable, however broken its measurement logic — and a broken
measurement path is a nuisance, while a broken update path is a trip to the bench.

Two configuration details for Phase A:

- `BOOTLOADER_DATA_FACTORY_RESET` defaults to `"nvs"` — decide explicitly whether the recovery
  button should also wipe NVS, or set it empty to keep settings.
- **Do not put the factory-reset GPIO on the BOOT strapping pin.** Holding BOOT at reset enters
  the ROM download mode (i.e. the USB path) before the second-stage bootloader ever runs, so
  the two would collide. Pick a free pin with its own button or jumper (P4 allows GPIO 0–54).

### 4. UDP replaces UART for measurement sync

- **Broadcast trigger**, unicast replies carrying an epoch/sequence number.
- A missing reply drops that node **for that run**; combine over √(healthy nodes). This is not
  new behaviour — `PLAN_4NODE` already specifies per-node degradation.
- **Keep the existing command semantics** (`P`/`B`/`M`/`D`/`A`) over the new transport for the
  first cut. The statistics layer then needs no changes at all, which keeps the transport
  swap independently testable.
- **Discovery by broadcast**, replacing the `P`→`OK` probe. No static IP table to maintain.

### 5. Consequences that follow automatically

- **Stall policy at n ≥ 3 becomes "drop the node, combine over n−1"** — `PLAN_4NODE`'s option
  (b), which it already anticipated. Aborting was the honest response at n=2 because losing one
  node halved the instrument; at n=4 it is needless.
- **Each node serves its own `/diag`**, so the `D`-command marshalling added in Phase 3 retires.
- The per-loop slave camera columns in `/loops` get their data from HTTP instead of UART.

## Reuse map — what already exists, so nothing is reinvented

Everything needed ships with ESP-IDF v6.0.1 locally. Verified present at
`C:\esp\v6.0.1\esp-idf\examples\`:

| Need | Take from | Notes |
|---|---|---|
| `esp_ota_begin/write/end/set_boot_partition` sequence | `system/ota/native_ota_example` (345 lines) | Also carries the exact rollback pattern: read `ESP_OTA_IMG_PENDING_VERIFY` at boot, run a `diagnostic()`, then `esp_ota_mark_app_valid_cancel_rollback()`. Its HTTP **client** is what we drop |
| Receiving a pushed image over HTTP | `protocols/http_server/file_serving` → `upload_post_handler()` | `req->content_len` + loop on `httpd_req_recv()` into a scratch buffer. Swap `fwrite` for `esp_ota_write` and that *is* `/update` |
| Version check, partial download, richer rollback | `system/ota/advanced_https_ota` | Worth reading even though we are not using HTTPS pull |
| Partition table shape | `system/ota/partitions_ota/*.csv` | Copy the layout, resize for 32 MB |
| Host-side partition inspection | `system/ota/otatool` | Useful for scripted verification |
| Ethernet bring-up | **our own `ethernet_init()` in `main/elotto.c`** | Already proven on this exact board (IP101GRI/RMII, GPIO 31/52/51). Better than `examples/ethernet/basic`, which is generic |

**Push, not pull.** Every IDF OTA example is *pull* — the device fetches from an HTTPS URL. Ours
is *push*: `curl --data-binary @app.bin http://<node>/update`. That needs no HTTP server on the
PC, reuses the webserver already running, and fits the existing permission rules. The cost is
that the two halves come from two different examples instead of one.

Genuinely new code is therefore small: the httpd→`esp_ota` glue (~80–100 lines), the boot-loop
counter (~30), and the factory-app scaffolding (`ethernet_init` + httpd + handlers). Call it
~300 lines, most of it adapted rather than written.

Third-party alternatives (ElegantOTA and similar) are Arduino-framework and would mean pulling
Arduino in as a component — not worth it against two first-party examples.

**Recovery bootloader is not available on this chip.** IDF v6 supports a
`bootloader, recovery` partition type, but `SOC_RECOVERY_BOOTLOADER_SUPPORTED` is absent from
`soc/esp32p4/include/soc/soc_caps.h`, so failure case 5 in §3a stays USB-only. Verified, not
assumed.

## Risks

1. **Shared PoE rail is a new correlation path.** Until now each board had its own USB supply
   and the noise sources were independent all the way down to power. Common-mode ripple is now
   the most plausible mechanism by which two cameras could correlate — and correlation is
   exactly what invalidates the ×√n gain. The detector already exists (6 pairwise r, flag
   |r|·√n > 3). **Run one node on separate USB power during bring-up as a control**: if the
   three PoE nodes correlate with each other but not with the isolated one, that is unambiguous.
2. **Do the boards actually accept PoE?** A PoE switch supplies power, but each board needs an
   onboard PD or a splitter. Verify before ordering anything.
3. **UDP loss must be handled explicitly**, not assumed away, even on a quiet dedicated switch.
4. **The partition migration is a one-way door**: full erase + serial flash on all four boards,
   losing NVS. Do it deliberately, not next to a session worth keeping.
5. **OTA cannot repair a bad bootloader or a changed partition table.** Those remain USB-only.
   USB is retired as a *workflow*, not removed as a *recovery* option — keep one cable.
6. **The update endpoint is unauthenticated on the house LAN** (the master is reachable at
   192.168.178.100 from a browser, so this is not an isolated network). A shared token in the
   request is the minimum; decide in Phase A rather than retrofitting.

## Phases

Each phase is one focused session with a gate. Do not start a phase before the previous gate
passes.

### Phase A — Flash size, partition table, and the OTA-Firmware

- `sdkconfig.defaults`: `FLASHSIZE_32MB`, custom partition table, rollback enabled. Per project
  rule: edit `sdkconfig.defaults`, delete `sdkconfig`, regenerate, verify the diff.
- Build the factory updater as its own project/app: Ethernet + HTTP + `esp_ota_ops` + the
  capability table above. Target ≤ 130 KB RAM in use.
- Install on **one** board via USB (erase + flash bootloader, partition table, factory, app).
  **Do the slave (COM6) first, not the master.** A bricked slave costs a reflash; a bricked
  master takes down the web UI and the whole measurement path. The slave is already cabled to
  the LAN, and since the updater brings up Ethernet by definition, installing it is also what
  first puts that node on the network.
- **Gate:** with the board on Ethernet only, push an app image over the network **twice in a
  row**, then prove each recovery path from §3a without a cable:
  1. flash an app that panics before validating → expect rollback to the previous app;
  2. flash an app that validates and *then* crash-loops → expect the boot-counter fallback to
     `factory`;
  3. hold the recovery GPIO at power-up → expect `otadata` erased and the updater booted.

  Report the measured RAM figure against the 130 KB budget. Recovery paths that have only been
  reasoned about do not count as passing — `PLAN_4NODE.md` already carries one such untested
  safety claim (the camera-stall abort), and that is one too many.

**Status: PASSED on the slave** (2026-07-25, MAC 80:f1:b2:d2:e3:e5, DHCP 192.168.178.103).
Gate 4 deferred — see below.

| | measured | budget |
|---|---|---|
| Static RAM (DIRAM) | **68.3 KB** (11.85 %) | ≤ 130 KB ✔ |
| Free heap at idle | 556 KB of 576 KB | — |
| Image size | 396 KB in the 1 MB `factory` slot (61 % free) | — |
| Update over Ethernet | 406 128 bytes in **1.59 s** | vs ~9 s over USB |
| Flash detected | 32 MB, partition table exactly as specified | — |

Gates, each observed on the console rather than inferred:

1. **Two updates back to back** — `factory` → `ota_0` → `ota_1`, alternating slots, each image
   validating itself once Ethernet was up (`ota_state: 2` = `ESP_OTA_IMG_VALID`).
2. **Rollback** — an image poisoned to abort *before* validating: booted `ota_0`, aborted,
   rebooted, and the bootloader came back up **on `ota_1`** with the network live. No cable.
3. **Boot counter** — an image poisoned to abort 5 s *after* validating (rollback disarmed by
   then): `boot attempt 1 … 2 … 3`, then `3 failed boots — falling back to factory updater`,
   `Defaulting to factory image`, and the recovery updater took over.

Notes worth keeping:

- **One binary, both roles works.** The same image runs as `factory` and as the OTA app; the
  role is decided by which partition it boots from. The poison hooks are skipped when running
  from `factory`, so the recovery image cannot poison itself into a loop — without that guard,
  gate 3 would have taken the node down permanently.
- The poison flag is one-shot for the *early* case and persistent for the *late* case,
  deliberately: a persistent early-crash flag would also kill the image being rolled back to,
  which would have conflated gates 2 and 3 into "everything dies".
- During each crash-loop iteration the node was reachable for ~5 s and could have been
  updated. The boot counter still fired, which is the correct bias: fall back to a known-good
  recovery image rather than depend on a human catching a 5 s window.

**Gate 4 (GPIO factory reset) is deferred, not skipped**, and `CONFIG_BOOTLOADER_FACTORY_RESET`
is intentionally left off. Naming the wrong pin is actively harmful — a pin held low by hardware
would erase `otadata` on *every* boot — and it must not be the BOOT strapping pin, since ROM
download mode wins before the second-stage bootloader runs. Needs a free pin on the Waveshare
header, with a button or jumper to ground. The three software paths above already cover every
failure except a *hang* with no reboot.

**Consequence to be aware of:** the slave now runs the updater, not the GCP slave firmware, and
its old 2 MB partition layout is gone. It cannot rejoin a measurement session until the slave
app gains Ethernet (Phase C) — because with rollback armed, an app that cannot be reached over
the network is by design rolled back on the next boot. Restoring the old firmware over USB is a
few minutes' work if the 2-node system is needed sooner.

### Phase B — OTA endpoint in the application

- `POST /update` in the app (fast path), running-image sha256 + build id in `/status`.
- **Refuse an update while `state == running`** — a flash mid-session destroys a measurement,
  and today nothing prevents that but discipline.
- Mark-valid only after the webserver answers.
- **Gate:** update the master over Ethernet during idle; verify the refusal path during a
  running session; confirm `/status` reports the new build id.

**Status: PASSED** (2026-07-25, master, 192.168.178.100).

- **752 528 bytes pushed over Ethernet in ~3.1 s**, four full cycles: `factory` → `ota_0` →
  `ota_1` → `factory` → `ota_0`, each image validating itself once the webserver answered.
  The master's own application now reaches the board without a cable.
- **`/status` carries firmware identity** — `fw_version`, `fw_built`, `fw_sha` (elf sha256),
  `fw_slot`, `fw_state`, `fw_boot_fails`. Without it, an update that answered `ok` could not be
  told apart from one that silently rolled back; that is not a hypothetical, see below.
- **Refusal during a session works and is machine-readable**: `HTTP 409 Conflict`,
  `busy: a session is running — abort it first`. IDF has no `HTTPD_409_CONFLICT`, so the status
  is set by hand rather than reusing 400/403 — a push script must be able to tell "refused, try
  later" from "your image is bad". The image was untouched afterwards (same `fw_sha`).
  **This is a real gain over USB**, where nothing but discipline stopped a flash from
  destroying a running measurement.

Implementation note: the update endpoint and all boot-safety logic moved into the shared
**`components/elotto_ota`**, used by the updater, the master and (Phase C) the slave. Three
copies of the code that decides whether a node is still reachable would be three chances to get
a recovery path subtly wrong on one node only. `ota_firmware` therefore points
`EXTRA_COMPONENT_DIRS` at that *single component*, not at `components/` — IDF compiles every
component it discovers, and pulling `elotto_camera` into the recovery image would drag in PSRAM
and the sensor driver, which is the opposite of the point.

Two pre-existing problems surfaced and were fixed on the way:

- **The master had no `sdkconfig.defaults`.** Its configuration had been built up interactively,
  so the documented rule ("edit `sdkconfig.defaults`, delete `sdkconfig`, regenerate") could not
  be followed without losing settings. One now exists, extracted from the working config and
  verified by diffing the regenerated `sdkconfig` against the old one — 18 keys changed, all
  either intended or IDF's own consequences of 32 MB flash (`BOOTLOADER_FLASH_32BIT_ADDR`).
- **The master's `CMakeLists.txt` never set `IDF_TARGET`**, so a clean checkout — or exactly the
  regenerate the config rule demands — failed with "CMAKE_C_COMPILER not set".

**One unexplained event, recorded rather than smoothed over:** the very first `factory` → `ota_0`
push booted the app and ended up back on `factory` with `boot_fails = 1`, i.e. the image failed
before validating and the bootloader recovered it. No console was attached at that moment, so
the cause is unknown. It did **not** reproduce: the identical transition, with the same binary
and console attached, has since run cleanly three times. Worth watching for on the remaining
nodes — and note that the safety net turned an unexplained failure into a non-event, which is
the entire argument for having it.

### Phase C — UDP transport at n=2 (the A/B)

- Slave gains Ethernet + a UDP command loop; master replaces the UART calls with broadcast
  trigger + reply collection. Same command semantics.
- Deliberately done at **n=2 first**, on the existing pair, so it is a controlled comparison
  rather than a rewrite entangled with scaling.
- **Gate:** a 2-node camera session over UDP reproduces the UART-era numbers — `pair_r` ≈ 0 at
  comparable n, σm/σs ≈ 1, `loop_sigma` ≈ 1, zero lost triggers. If the statistics move, the
  transport changed something it should not have.

**Status: PASSED** (2026-07-25). Same configuration as the UART-era session it is compared
against — 6/49, Loops=3, Runs=200, both nodes `src=cam`, n = 600 pairs — so this is a like-for-
like A/B, not a fresh measurement that happens to look reasonable.

| | **UDP (this phase)** | UART (PLAN_4NODE Phase 2) |
|---|---|---|
| `pair_r` (n=600) | **+0.0650** (\|r\|·√n = 1.59) | −0.0201 (\|r\|·√n = 0.49) |
| σm / σs | **1.0512 / 1.0027** | 1.0141 / 1.0356 |
| `loop_sigma` per loop | **1.0760 / 1.0695 / 1.0342** | 0.9721 / 0.9668 / 1.0992 |
| camera stalls M/S | **0 / 0** | 0 |
| lost triggers | **0** (`net_retries` 0, `net_stale` 0) | — |
| session wall time | 12.1 min | 35.4 min |

- **Zero lost datagrams across ~1390 command round trips** (300 baseline + 490 scoring + 600
  measurement, plus discovery and three per-loop `D` queries). Not "it felt reliable": the
  master counts `net_retries` / `net_lost` / `net_stale` per session and publishes them in
  `/status`, because Risk 3 says loss must be handled explicitly rather than assumed away.
- **`pair_r` is consistent with chance.** \|r\|·√n = 1.59 sits well under the project's own flag
  threshold of 3 (two-sided p ≈ 0.11). Worth recording how it got there: after loop 1 it read
  +0.147 at n=200 (\|r\|·√n = 2.1), then fell to +0.102 at n=400 and +0.065 at n=600 — the
  regression toward zero that a sampling fluctuation produces and a real coupling does not.
  Judging it at n=200 would have raised a false alarm; that is exactly why the gate specifies
  "comparable n".
- **Per-node σ ≈ 1 on both**, and every loop's combined σ lands inside the UART era's own spread
  (0.967–1.099). The transport did not touch the statistics, which is the whole claim.
- **The run is ~3× faster** than the UART session at identical settings. That is not the
  transport: `SCORE_REPS` was lowered from 40 to 10 in the meantime (PLAN_4NODE Phase 0), so the
  one-time scoring phase is a quarter as long. Per measurement the pace is ~0.53 s, i.e. the
  camera run length — the UDP round trip is not measurable against it.

**Abort was proved, not argued** (PLAN_4NODE already carries one untested safety claim; that is
one too many). Aborting mid-baseline stopped the slave inside its run (`measuring` went false on
its own `/diag`), and a fresh session started afterwards ran its full baseline and entered
scoring with `net_stale` still 0 — i.e. neither the master's queued abort-acknowledgements nor a
leftover `A` on the slave leaked into the next session. Both ends drain their socket when a
session starts, for the reason the UART path called `uart_flush_input()`.

Two implementation notes worth keeping:

- **The sequence number is the load-bearing part, not the port number.** UART was lossless and
  ordered, so a reply could only belong to the command just sent. UDP guarantees neither, and a
  late reply accepted blindly would pair `z_slave` of run *k* with `z_master` of run *k+1* —
  a correlation bug that looks exactly like physics, in the very quantity `pair_r` exists to
  detect. Mismatched frames are therefore dropped and counted, never used. A timed-out command
  is resent under the *same* sequence number and the slave answers a repeat of a completed
  command from a one-entry cache, so a lost reply costs a round trip while a lost command is
  re-executed exactly once.
- **The slave needed a webserver before it could be installed at all.** With rollback armed, an
  image that cannot be reached over the network is reverted by design — so the Ethernet + httpd
  work was not a bonus feature of Phase C but its precondition. The slave has run the recovery
  updater since Phase A for exactly this reason, and now serves its own `/diag` (PLAN_NETWORK §5
  anticipated this) alongside the shared `/update`.

**One number to watch, deliberately not smoothed over:** `drift_t` came out at 3.15 with a slope
of +0.021 z/loop, nominally over the |t| > 3 flag. It should not be read as drift. The
regression has three points (df = 1), where any monotone sequence yields a large t almost by
construction, and the underlying offsets (−0.084 → −0.051 → −0.041 z/run) move by less than the
per-loop noise. PLAN_4NODE makes the same caveat about its own 3-loop check. A long session is
what would settle it.

**The slave's UART firmware is gone**, and with it the crossover link. `SLAVE_BAUD` /
`UART_BAUD` and the GPIO14/15 wiring no longer exist in either repo.

### Phase D — Scale to four nodes

- `slaves[]` (ip, last_seen, ok); broadcast `B`/`M`/`A`; per-node timeout.
- Generalize `PairStats` to all 6 pairs; publish per-node σ and max |r|.
- Stall policy → drop-and-continue over n−1.
- UI: "4-node • SNR ×2" badge, per-node health/σ row.
- **Gate:** 4-node session; all 6 pairwise r ≈ 0; combined σ ≈ 1; unplug one node mid-run and
  see it degrade to √3 with a UI flag, not a crash. Plus the PoE correlation control from
  Risk 1.

### Phase E — Retire USB, and the docs

- README: 4-node topology, switch wiring, OTA workflow, recovery procedure.
- `CLAUDE.md`: build/flash instructions become `curl … /update`; keep the USB recovery route
  documented for bootloader/partition changes.
- `PLAN_4NODE.md`: mark Phase 4 superseded, pointing here.
- Version bump.

## Hardware checklist

- 4× ESP32-P4-ETH — **present**
- 4× OV5647, one per node, never shared — **present**
- 4-port PoE switch — **present**
- PoE powering path per board (onboard PD or splitter) — **verify** (Risk 2)
- 4× Ethernet cables; one USB cable retained for recovery
- Light-tight capping per camera; consider giving the dimmer units *more* light, not less
  (see `PLAN_4NODE.md` Phase 2 — bias tracks `mean_px`)

## Workflow

Planning: this document is the contract. Implementation: one phase per session, prompt
*"Implement Phase X of docs/PLAN_NETWORK.md"*. Commit at every green gate. The
`components/elotto_camera` component is shared between the repos — when it changes, build,
flash and commit every node together.
