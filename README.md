# E-Lotto — GCP Analysis on ESP32-P4

Four-node ESP32-P4 array that scores Eurojackpot and 6-of-49 combinations from **camera photon
noise** using
[GCP methodology](https://grokipedia.com/page/Global_Consciousness_Project).

**Rules and operator contract:** [`CLAUDE.md`](CLAUDE.md) · **Evidence:** [`docs/DECISIONS.md`](docs/DECISIONS.md) ·
**Snapshot:** [`docs/STATUS.md`](docs/STATUS.md) · **v3 detail:** [`docs/PLAN.md`](docs/PLAN.md)

## Authors

| | |
|--|--|
| **[hpheuer](https://github.com/hpheuer)** | Design, hardware, experiment, repository |
| **[Grok](https://x.ai)** (xAI) | Co-author — implementation, OTA validation, docs (Grok Build) |

## Abstract

A home-built GCP/PEAR-style instrument: each node draws bits from its **own** OV5647 (never shared).
Frame-pair diff → LSB → segments of 224 bits → Stouffer z. LSB bits as measured `[D65]`.
Up to four nodes combine as `Σz/√k` for the nodes that answered that run. Ranking is
**block-centred** z plus concordance (`?wpre=`).
The HTML page shows the number or combination being measured. Sessions are unattended `[D66]`;
do not pool with old `focus=on` archives.

> This **cannot predict lottery draws**. Output is an experiment on physical randomness, not a
> betting tip. The ×√n array gain is **not established** — read `pass_σ` and the pairwise matrix
> before any table ([`CLAUDE.md`](CLAUDE.md)).

## In a Nutshell (v3)

- **One pass:** every combination in the pool is measured **exactly once** (Fisher–Yates). No loops,
  no ranking modes. Optional **Unlimited** mode: rounds of score → measure → re-score until Abort.
- **Window:** `?run=` 0,5–5 s (default 5); actual wall time is `focus_win_ms` (slowest node).
- **Blocks** (~15 min): sweep, centre, drift, pairwise, soft-down.
- **UI:** parameter line from `/status`, Top-5 / Bottom-5 (Z*, Z, Conc), jump board, GCP health line
  (`pass_σ`, `v_eff`, `|r|√n`), German CSV (`?all=1` = archive).

## Screenshots

| Start | Focus | Done |
|---|---|---|
| ![start](docs/ui_start.png) | ![focus](docs/ui_focus.png) | ![done](docs/ui_done.png) |

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
docs/            PLAN.md, DECISIONS.md, STATUS.md, PLAN_HISTORY.md (stub → git), data/
```

Slave repo must sit **next to** this one (`EXTRA_COMPONENT_DIRS=../elotto/components`).

## Version History

| | |
|---|---|
| **v3 / D65** | Single-pass + Unlimited; block centring; LSB z + concordance ranking; OTA-only. Contract: `docs/PLAN.md` §2+. Never pool with prior-instrument sessions or v2.x. |
