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
- **Gate:** with the board on Ethernet only, push an app image over the network **twice in a
  row**; then deliberately flash a broken app (e.g. one that never brings up Ethernet) and show
  it recovers to a usable updater **without a cable**. Report the measured RAM figure against
  the 130 KB budget.

### Phase B — OTA endpoint in the application

- `POST /update` in the app (fast path), running-image sha256 + build id in `/status`.
- **Refuse an update while `state == running`** — a flash mid-session destroys a measurement,
  and today nothing prevents that but discipline.
- Mark-valid only after the webserver answers.
- **Gate:** update the master over Ethernet during idle; verify the refusal path during a
  running session; confirm `/status` reports the new build id.

### Phase C — UDP transport at n=2 (the A/B)

- Slave gains Ethernet + a UDP command loop; master replaces the UART calls with broadcast
  trigger + reply collection. Same command semantics.
- Deliberately done at **n=2 first**, on the existing pair, so it is a controlled comparison
  rather than a rewrite entangled with scaling.
- **Gate:** a 2-node camera session over UDP reproduces the UART-era numbers — `pair_r` ≈ 0 at
  comparable n, σm/σs ≈ 1, `loop_sigma` ≈ 1, zero lost triggers. If the statistics move, the
  transport changed something it should not have.

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
