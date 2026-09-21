# E-Lotto — GCP Analysis on ESP32-P4

Four-node ESP32-P4 array that scores Eurojackpot and 6-of-49 combinations from **camera photon
noise** using
[GCP methodology](https://grokipedia.com/page/Global_Consciousness_Project).

**Rules and operator contract:** [`CLAUDE.md`](CLAUDE.md) · **Evidence:** [`docs/DECISIONS.md`](docs/DECISIONS.md) ·
**Snapshot:** [`docs/STATUS.md`](docs/STATUS.md)

## Authors

| | |
|--|--|
| **[hpheuer](https://github.com/hpheuer)** | Design, hardware, experiment, repository |
| **[Grok](https://x.ai)** (xAI) | Co-author — implementation, OTA validation, docs (Grok Build) |

## Abstract

A home-built GCP/PEAR-style instrument: each node draws bits from its **own** camera (OV5647 or IMX219, never shared).
Frame-pair diff → LSB → segments of 224 bits → Stouffer z. LSB bits as measured `[D65]`.
Up to four nodes combine as `Σz/√k` for the nodes that answered that run. Ranking is
**block-centred** z plus concordance (`?wpre=`), Z* in units of that block's σ `[D68]`.
Scoring uses the same per-node centre `[D69]`.
The HTML page shows the number or combination being measured. Sessions are unattended `[D66]`;
do not pool with old `focus=on` archives.

> This **cannot predict lottery draws**. Output is an experiment on physical randomness, not a
> betting tip. The ×√n array gain is **not established** — read `pass_σ` and the pairwise matrix
> before any table ([`CLAUDE.md`](CLAUDE.md)).

## In a Nutshell (v3)

- **Rounds until Abort:** score → measure a cap-sized pool → re-score. No loops, no ranking modes.
- **Window:** `?run=` 0,5–5 s (default 5); actual wall time is `focus_win_ms` (slowest node).
- **Blocks:** one round = one block (`?maxruns=`, default 100). Sweep, centre, drift, pairwise,
  soft-down at the round boundary.
- **UI:** parameter line from `/status`, one sortable Top-10 (Z*, Z, Conc, Δn), jump board,
  GCP health line (`pass_σ`, `v_eff`, `|r|√n`), German CSV (`?all=1` = archive).
- **Illumination:** `tools/tune.html` — live per-node linearity/sweep board (idle only).

## Screenshots

| Start | Session | Results |
|---|---|---|
| ![start](docs/ui_start.png) | ![session](docs/ui_focus.png) | ![results](docs/ui_done.png) |

## Hardware

| node | address | role |
|---|---|---|
| master | 192.168.178.100 | web UI, session, combine |
| slave0 | 192.168.178.103 | measure |
| slave1 | 192.168.178.145 | measure |
| slave2 | 192.168.178.155 | measure |

All four: Waveshare ESP32-P4-ETH, **PoE**, own OV5647, lit enclosure (not dark) `[D28]`. Never power
the lamp from a node's VSYS `[D29]`. UDP discovery on port 5000 — addresses are informational.
PSRAM mandatory. USB = recovery only.

## Build & Flash

```powershell
cd D:\E-Lotto\elotto
.\build.ps1 build                        # master
.\build.ps1 -C ../elotto_slave build     # slave
.\build.ps1 -C ota_firmware build        # factory updater

curl.exe http://192.168.178.100/update --data-binary @build/elotto.bin
curl.exe http://192.168.178.103/update --data-binary @../elotto_slave/build/elotto_slave.bin
# same for .145 / .155 — abort any session first (409 while running)
```

After OTA, poll `fw_sha` in `/status` until it **changes**. Fresh board: USB erase-flash of
`ota_firmware`, then Ethernet forever. Details, recovery, diagnostics: [`CLAUDE.md`](CLAUDE.md).

## Project structure

```
main/            elotto.c (UI/HTTP), sensor.c/h, nodes.c/h, focus.c
components/      elotto_camera, elotto_gcp, elotto_link, elotto_ota  (shared with slave)
ota_firmware/    recovery image (factory)
tools/           tune.html (live illumination board)
docs/            DECISIONS.md, STATUS.md; PLAN.md / PLAN_HISTORY.md (stubs → git); data/
```

Slave repo must sit **next to** this one (`EXTRA_COMPONENT_DIRS=../elotto/components`).

## Version History

| | |
|---|---|
| **v3 / D67** | Rounds until Abort; block = round (D76); LSB z + centred-half concordance (D77); sortable Top-10 (D78). Contract: `CLAUDE.md`. Never pool with prior-instrument sessions or v2.x. |
